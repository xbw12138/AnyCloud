//
// cloud: account card + provider actions page.
//

#include "cloud/pages/profile_page.hpp"

#include <algorithm>

#include <cpr/async.h>

#include "cloud/drive/drive_manager.hpp"
#include "cloud/main_activity.hpp"
#include "cloud/pages/http_log_page.hpp"
#include "cloud/pages/info_page.hpp"
#include "cloud/pages/login_page.hpp"
#include "cloud/update_check.hpp"
#include "nxkit/http_client.hpp"

#ifndef NXKIT_APP_VERSION
#define NXKIT_APP_VERSION "unknown"
#endif

#ifdef BUILTIN_NSP
#include <switch.h>
#include "nspmini.hpp"
#include <borealis/views/dialog.hpp>
#endif

using namespace brls::literals;

namespace cloud {

namespace {

std::string formatBytes(uint64_t bytes) {
    char buf[64];
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(buf, sizeof(buf), "%.2f GB",
                 (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else {
        snprintf(buf, sizeof(buf), "%.0f MB",
                 (double)bytes / (1024.0 * 1024.0));
    }
    return buf;
}

/// First UTF-8 character of `text`, used as the avatar initial.
std::string firstCharacter(const std::string& text) {
    if (text.empty()) return "?";
    size_t end = 1;
    while (end < text.size() && (text[end] & 0xC0) == 0x80) end++;
    return text.substr(0, end);
}

#ifdef BUILTIN_NSP
// 桌面图标由独立的转发器标题提供。01A0 是 AnyCloud 独占段：
// 0101-0104 已被 Switch小程序/应用商店 等项目占用，用那些号会被主菜单
// 当成别的软件（图标、名称都显示成对方的）。
constexpr u64 kForwarderTitleId = 0x010FF000FFFF01A0ULL;
#endif

}  // namespace

/// Storage usage bar: faint rounded track with an accent fill.
class StorageBar : public brls::View {
public:
    void setProgress(float value) {
        float next = std::min(1.0f, std::max(0.0f, value));
        if (next == this->progress) return;
        this->progress = next;
        this->invalidate();
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (width <= 0 || height <= 0) return;

        brls::Theme theme = brls::Application::getTheme();
        float radius = height / 2.0f;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, radius);
        nvgFillColor(vg, this->a(theme["cloud/border"]));
        nvgFill(vg);

        if (this->progress <= 0) return;

        // Keep the fill at least as wide as it is tall so the rounded cap
        // stays well formed for tiny usage values.
        float fillWidth = std::max(height, width * this->progress);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, fillWidth, height, radius);
        nvgFillColor(vg, this->a(theme["brls/accent"]));
        nvgFill(vg);
    }

private:
    float progress = 0;
};

/// Circular remote avatar. Draws nothing until the image bytes arrive, so the
/// accent circle and initial behind it remain visible as a fallback.
class AvatarView : public brls::View {
public:
    void setImageData(const std::string& bytes) {
        this->bytes = bytes;
        this->failed = false;
        this->stale = true;
        this->invalidate();
    }

    void clear() {
        if (this->bytes.empty() && this->texture == 0) return;
        this->bytes.clear();
        this->stale = false;
        this->failed = false;
        this->texture = 0;
        this->invalidate();
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (width <= 0 || height <= 0 || this->bytes.empty()) return;

        if (this->stale) {
            this->stale = false;
            this->texture =
                nvgCreateImageMem(vg, 0,
                                  reinterpret_cast<unsigned char*>(
                                      &this->bytes[0]),
                                  (int)this->bytes.size());
            if (this->texture <= 0) {
                this->failed = true;
                return;
            }
            nvgImageSize(vg, this->texture, &this->imageWidth,
                         &this->imageHeight);
        }
        if (this->failed || this->texture <= 0 || this->imageWidth <= 0 ||
            this->imageHeight <= 0)
            return;

        float cx = x + width * 0.5f;
        float cy = y + height * 0.5f;
        float radius = std::min(width, height) * 0.5f;

        // Aspect-fill the square image inside the circle.
        float scale = std::max((radius * 2.0f) / this->imageWidth,
                               (radius * 2.0f) / this->imageHeight);
        float drawWidth = this->imageWidth * scale;
        float drawHeight = this->imageHeight * scale;

        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, radius);
        nvgFillPaint(vg, nvgImagePattern(vg, cx - drawWidth / 2.0f,
                                        cy - drawHeight / 2.0f, drawWidth,
                                        drawHeight, 0.0f, this->texture, 1.0f));
        nvgFill(vg);
    }

private:
    std::string bytes;
    int texture = 0;
    int imageWidth = 0;
    int imageHeight = 0;
    bool stale = false;
    bool failed = false;
};

/// Two opposite arrows (a "swap" glyph) drawn with NanoVG: no icon font
/// involved, so it always renders.
class SwapArrowsView : public brls::View {
public:
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (width <= 0 || height <= 0) return;

        brls::Theme theme = brls::Application::getTheme();

        const float midY = y + height * 0.5f;
        const float gap = height * 0.22f;
        const float inset = width * 0.10f;
        const float left = x + inset;
        const float right = x + width - inset;
        const float head = 3.6f;

        nvgStrokeColor(vg, this->a(theme["cloud/text_secondary"]));
        nvgStrokeWidth(vg, 2.0f);
        nvgLineCap(vg, NVG_ROUND);
        nvgLineJoin(vg, NVG_ROUND);

        // Top line with the arrow head on the right: →
        nvgBeginPath(vg);
        nvgMoveTo(vg, left, midY - gap);
        nvgLineTo(vg, right, midY - gap);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, right - head, midY - gap - head);
        nvgLineTo(vg, right, midY - gap);
        nvgLineTo(vg, right - head, midY - gap + head);
        nvgStroke(vg);

        // Bottom line with the arrow head on the left: ←
        nvgBeginPath(vg);
        nvgMoveTo(vg, left, midY + gap);
        nvgLineTo(vg, right, midY + gap);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, left + head, midY + gap - head);
        nvgLineTo(vg, left, midY + gap);
        nvgLineTo(vg, left + head, midY + gap + head);
        nvgStroke(vg);
    }
};

/// Flat action row: label on the left, chevron on the right.
class ActionTile : public brls::Box {
public:
    ActionTile(const std::string& title, std::function<void()> callback)
        : callback(std::move(callback)) {
        brls::Theme theme = brls::Application::getTheme();

        this->setAxis(brls::Axis::ROW);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setHeight(64);
        this->setPadding(0, 24, 0, 24);
        this->setCornerRadius(16);
        this->setHighlightCornerRadius(16);
        this->setBorderColor(theme["brls/accent"]);
        this->setBorderThickness(0);
        this->setFocusable(true);

        this->title = new brls::Label();
        this->title->setText(title);
        this->title->setFontSize(18);
        this->title->setTextColor(theme["cloud/text"]);
        this->title->setHeight(24);
        this->title->setGrow(1);
        this->addView(this->title);

        this->chevron = new brls::Label();
        this->chevron->setText("›");
        this->chevron->setFontSize(22);
        this->chevron->setTextColor(theme["cloud/text_secondary"]);
        this->chevron->setHeight(24);
        this->addView(this->chevron);

        this->registerAction("cloud/profile/open"_i18n, brls::BUTTON_A,
                             [this](brls::View*) {
                                 if (this->callback) this->callback();
                                 return true;
                             });
        this->refreshStyle();
    }

    void setTitle(const std::string& text) {
        if (!this->title) return;
        this->title->setText(text);
    }

    void onFocusGained() override {
        brls::Box::onFocusGained();
        this->refreshStyle();
    }

    void onFocusLost() override {
        brls::Box::onFocusLost();
        this->refreshStyle();
    }

private:
    void refreshStyle() {
        brls::Theme theme = brls::Application::getTheme();
        bool focused = this->isFocused();
        this->setBackgroundColor(focused ? theme["cloud/surface_alt"]
                                         : theme["cloud/surface"]);
        this->setBorderThickness(focused ? 2.0f : 0.0f);
    }

    brls::Label* title = nullptr;
    brls::Label* chevron = nullptr;
    std::function<void()> callback;
};

/// Lists every published build with its changelog, newest first, straight from
/// the store backend.
class ReleaseHistoryView : public brls::Box {
public:
    ReleaseHistoryView() {
        brls::Theme theme = brls::Application::getTheme();
        this->setAxis(brls::Axis::COLUMN);
        this->setGrow(1);
        this->setPadding(26, 40, 20, 40);
        this->setBackgroundColor(theme["cloud/bg"]);

        brls::Box* header = new brls::Box(brls::Axis::ROW);
        header->setAlignItems(brls::AlignItems::CENTER);

        brls::Label* title = new brls::Label();
        title->setText("cloud/profile/changelog"_i18n);
        title->setFontSize(28);
        title->setTextColor(theme["cloud/text"]);
        title->setHeight(36);
        header->addView(title);

        brls::Label* hint = new brls::Label();
        hint->setText("cloud/profile/changelog_hint"_i18n);
        hint->setFontSize(14);
        hint->setTextColor(theme["cloud/text_secondary"]);
        hint->setHeight(20);
        hint->setGrow(1);
        hint->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        header->addView(hint);

        this->addView(header);

        this->list = new brls::Box(brls::Axis::COLUMN);
        this->list->setWidthPercentage(100);
        this->list->setPaddingTop(10);
        this->list->setPaddingBottom(20);
        this->list->addView(this->makeNotice("cloud/profile/changelog_loading"_i18n));

        brls::ScrollingFrame* scroller = new brls::ScrollingFrame();
        scroller->setGrow(1);
        scroller->setMarginTop(8);
        scroller->setContentView(this->list);
        this->addView(scroller);

        // Borealis does not pop activities on B by itself.
        this->registerAction("hints/back"_i18n, brls::BUTTON_B,
                             [](brls::View*) {
                                 brls::Application::popActivity();
                                 return true;
                             });

        // pushActivity only focuses the content view, so the scroller has to
        // take the focus explicitly or the stick never scrolls the page.
        brls::sync([scroller] { brls::Application::giveFocus(scroller); });

        fetchReleaseHistory([this, alive = this->alive](
                                bool ok, const std::vector<ReleaseNote>& notes,
                                const std::string& error) {
            brls::sync([this, alive, ok, notes, error] {
                if (!*alive) return;
                this->fill(ok, notes, error);
            });
        });
    }

    ~ReleaseHistoryView() override { *this->alive = false; }

private:
    brls::Label* makeNotice(const std::string& text) {
        brls::Theme theme = brls::Application::getTheme();
        brls::Label* label = new brls::Label();
        label->setText(text);
        label->setFontSize(16);
        label->setWidthPercentage(100);
        label->setSingleLine(false);
        label->setTextColor(theme["cloud/text_secondary"]);
        return label;
    }

    brls::Box* makeBlock(const ReleaseNote& note) {
        brls::Theme theme = brls::Application::getTheme();
        brls::Box* block = new brls::Box(brls::Axis::COLUMN);
        block->setWidthPercentage(100);
        block->setPadding(16, 20, 16, 20);
        block->setMarginBottom(10);
        block->setCornerRadius(16);
        block->setBackgroundColor(theme["cloud/surface"]);

        brls::Box* row = new brls::Box(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);

        brls::Label* version = new brls::Label();
        version->setText(note.version);
        version->setFontSize(20);
        version->setTextColor(theme["cloud/text"]);
        version->setHeight(26);
        row->addView(version);

        if (!note.date.empty()) {
            brls::Label* date = new brls::Label();
            date->setText(note.date);
            date->setFontSize(14);
            date->setMarginLeft(12);
            date->setGrow(1);
            date->setHeight(20);
            date->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
            date->setTextColor(theme["cloud/text_secondary"]);
            row->addView(date);
        }
        block->addView(row);

        if (!note.changelog.empty()) {
            brls::Label* body = new brls::Label();
            body->setText(note.changelog);
            body->setFontSize(15);
            body->setLineHeight(1.5f);
            body->setMarginTop(8);
            body->setWidthPercentage(100);
            body->setSingleLine(false);
            body->setTextColor(theme["cloud/text"]);
            block->addView(body);
        }
        return block;
    }

    void fill(bool ok, const std::vector<ReleaseNote>& notes,
              const std::string& error) {
        if (!this->list) return;
        this->list->clearViews();
        if (!ok) {
            this->list->addView(this->makeNotice(
                error.empty() ? "cloud/profile/changelog_failed"_i18n : error));
            return;
        }
        if (notes.empty()) {
            this->list->addView(
                this->makeNotice("cloud/profile/changelog_empty"_i18n));
            return;
        }
        for (const ReleaseNote& note : notes)
            this->list->addView(this->makeBlock(note));
    }

    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    brls::Box* list = nullptr;
};

ProfilePage::ProfilePage() {
    brls::Theme theme = brls::Application::getTheme();
    this->setAxis(brls::Axis::COLUMN);
    this->setPadding(24, 48, 24, 48);
    this->setBackgroundColor(theme["cloud/bg"]);
    this->build();
    this->driveGeneration = DriveManager::instance().generation();
    // Popping a login page back onto this tab does not fire willAppear, so the
    // card also follows the drive generation and reloads as soon as a login
    // flips the account state.
    this->driveSubscription = brls::Application::getRunLoopEvent()->subscribe(
        [this, alive = this->alive] {
            if (!*alive) return;
            auto& manager = DriveManager::instance();
            if (manager.generation() == this->driveGeneration) return;
            this->driveGeneration = manager.generation();
            this->reload();
        });
    this->refreshAccount();
}

ProfilePage::~ProfilePage() {
    *this->alive = false;
    brls::Application::getRunLoopEvent()->unsubscribe(this->driveSubscription);
}

void ProfilePage::build() {
    brls::Theme theme = brls::Application::getTheme();

    // --- Account card -------------------------------------------------------
    // The active drive is shown by the badge in the tab bar, so the card can
    // start right at the top of the page.
    brls::Box* card = new brls::Box(brls::Axis::COLUMN);
    card->setPadding(28, 30, 26, 30);
    card->setCornerRadius(24);
    card->setBackgroundColor(theme["cloud/surface"]);

    brls::Box* headerRow = new brls::Box(brls::Axis::ROW);
    headerRow->setAlignItems(brls::AlignItems::CENTER);

    brls::Box* avatar = new brls::Box(brls::Axis::COLUMN);
    avatar->setDimensions(76, 76);
    avatar->setCornerRadius(38);
    avatar->setAlignItems(brls::AlignItems::CENTER);
    avatar->setJustifyContent(brls::JustifyContent::CENTER);
    avatar->setBackgroundColor(theme["brls/accent"]);
    avatar->setMarginRight(24);

    this->avatarLabel = new brls::Label();
    this->avatarLabel->setFontSize(30);
    this->avatarLabel->setTextColor(nvgRGB(255, 255, 255));
    this->avatarLabel->setHeight(36);
    this->avatarLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    avatar->addView(this->avatarLabel);

    // Remote avatar drawn on top of the accent circle / initial fallback.
    this->avatarView = new AvatarView();
    this->avatarView->setPositionType(brls::PositionType::ABSOLUTE);
    this->avatarView->setPositionTop(0);
    this->avatarView->setPositionLeft(0);
    this->avatarView->setDimensions(76, 76);
    avatar->addView(this->avatarView);
    headerRow->addView(avatar);

    brls::Box* nameColumn = new brls::Box(brls::Axis::COLUMN);
    nameColumn->setGrow(1);

    this->nicknameLabel = new brls::Label();
    this->nicknameLabel->setFontSize(28);
    this->nicknameLabel->setTextColor(theme["cloud/text"]);
    this->nicknameLabel->setHeight(34);
    this->nicknameLabel->setSingleLine(true);
    nameColumn->addView(this->nicknameLabel);

    this->memberLabel = new brls::Label();
    this->memberLabel->setFontSize(14);
    this->memberLabel->setTextColor(theme["cloud/text_secondary"]);
    this->memberLabel->setHeight(18);
    this->memberLabel->setMarginTop(4);
    this->memberLabel->setSingleLine(true);
    nameColumn->addView(this->memberLabel);

    headerRow->addView(nameColumn);

    // Drive switcher: icon in the top-right corner of the account card.
    this->switchButton = new brls::Box(brls::Axis::ROW);
    this->switchButton->setAlignItems(brls::AlignItems::CENTER);
    this->switchButton->setJustifyContent(brls::JustifyContent::CENTER);
    this->switchButton->setDimensions(40, 40);
    this->switchButton->setMarginLeft(12);
    this->switchButton->setCornerRadius(20);
    this->switchButton->setHighlightCornerRadius(20);
    this->switchButton->setBackgroundColor(theme["cloud/surface_alt"]);
    this->switchButton->setFocusable(true);

    SwapArrowsView* arrows = new SwapArrowsView();
    arrows->setDimensions(22, 18);
    this->switchButton->addView(arrows);

    this->switchButton->registerAction(
        "cloud/profile/switch"_i18n, brls::BUTTON_A, [this](brls::View*) {
            this->openProviderMenu();
            return true;
        });
    this->switchButton->registerClickAction([this](brls::View*) {
        this->openProviderMenu();
        return true;
    });
    headerRow->addView(this->switchButton);

    card->addView(headerRow);

    // Storage usage block, hidden while signed out or before capacity loads.
    this->capacityBox = new brls::Box(brls::Axis::COLUMN);
    this->capacityBox->setMarginTop(26);

    brls::Box* capacityRow = new brls::Box(brls::Axis::ROW);
    capacityRow->setAlignItems(brls::AlignItems::CENTER);

    brls::Label* capacityTitle = new brls::Label();
    capacityTitle->setText("cloud/profile/capacity"_i18n);
    capacityTitle->setFontSize(14);
    capacityTitle->setTextColor(theme["cloud/text_secondary"]);
    capacityTitle->setHeight(18);
    capacityTitle->setGrow(1);
    capacityRow->addView(capacityTitle);

    this->capacityValue = new brls::Label();
    this->capacityValue->setFontSize(14);
    this->capacityValue->setTextColor(theme["cloud/text_secondary"]);
    this->capacityValue->setHeight(18);
    capacityRow->addView(this->capacityValue);
    this->capacityBox->addView(capacityRow);

    this->capacityBar = new StorageBar();
    this->capacityBar->setHeight(8);
    this->capacityBar->setMarginTop(12);
    this->capacityBar->setWidthPercentage(100);
    this->capacityBox->addView(this->capacityBar);

    card->addView(this->capacityBox);
    this->addView(card);

    // --- Actions ------------------------------------------------------------
    brls::Box* actions = new brls::Box(brls::Axis::ROW);
    actions->setMarginTop(20);

    this->refreshTile =
        new ActionTile("cloud/profile/refresh"_i18n, [this] { this->refreshAccount(); });
    this->refreshTile->setWidth(0);
    this->refreshTile->setGrow(1);
    actions->addView(this->refreshTile);

    brls::Box* gap = new brls::Box();
    gap->setWidth(16);
    gap->setHeight(1);
    actions->addView(gap);

    this->accountTile =
        new ActionTile("cloud/profile/logout"_i18n, [this] {
            if (this->loggedIn)
                this->logout();
            else
                this->openLogin();
        });
    this->accountTile->setWidth(0);
    this->accountTile->setGrow(1);
    actions->addView(this->accountTile);

    this->addView(actions);

    // --- Diagnostics --------------------------------------------------------
    // Request log and the home-menu shortcut share one row.
    brls::Box* tools = new brls::Box(brls::Axis::ROW);
    tools->setMarginTop(16);

    ActionTile* logTile = new ActionTile("cloud/log/title"_i18n, [] {
        brls::Application::pushActivity(new HttpLogPage());
    });
    logTile->setWidth(0);
    logTile->setGrow(1);
    tools->addView(logTile);

    brls::Box* updateGap = new brls::Box();
    updateGap->setWidth(16);
    updateGap->setHeight(1);
    tools->addView(updateGap);

    this->updateTile = new ActionTile("cloud/profile/check_update"_i18n,
                                      [this] { this->checkUpdate(); });
    this->updateTile->setWidth(0);
    this->updateTile->setGrow(1);
    tools->addView(this->updateTile);

    brls::Box* historyGap = new brls::Box();
    historyGap->setWidth(16);
    historyGap->setHeight(1);
    tools->addView(historyGap);

    ActionTile* historyTile =
        new ActionTile("cloud/profile/changelog"_i18n, [] {
            brls::Application::pushActivity(
                new brls::Activity(new ReleaseHistoryView()));
        });
    historyTile->setWidth(0);
    historyTile->setGrow(1);
    tools->addView(historyTile);

#ifdef BUILTIN_NSP
    brls::Box* toolsGap = new brls::Box();
    toolsGap->setWidth(16);
    toolsGap->setHeight(1);
    tools->addView(toolsGap);

    // Install the NSP forwarder so the user can add a home-menu icon.
    this->desktopTile = new ActionTile("cloud/profile/desktop"_i18n,
                                       [this] { this->installDesktopIcon(); });
    this->desktopTile->setWidth(0);
    this->desktopTile->setGrow(1);
    tools->addView(this->desktopTile);
#endif

    this->addView(tools);

    // --- Info row -----------------------------------------------------------
    brls::Box* info = new brls::Box(brls::Axis::ROW);
    info->setMarginTop(16);

    ActionTile* featuresTile =
        new ActionTile("cloud/profile/features"_i18n, [] {
            brls::Application::pushActivity(
                new InfoPage(InfoPage::Section::Features));
        });
    featuresTile->setWidth(0);
    featuresTile->setGrow(1);
    info->addView(featuresTile);

    brls::Box* infoGap = new brls::Box();
    infoGap->setWidth(16);
    infoGap->setHeight(1);
    info->addView(infoGap);

    ActionTile* controlsTile =
        new ActionTile("cloud/profile/controls"_i18n, [] {
            brls::Application::pushActivity(
                new InfoPage(InfoPage::Section::Controls));
        });
    controlsTile->setWidth(0);
    controlsTile->setGrow(1);
    info->addView(controlsTile);

    brls::Box* infoGap2 = new brls::Box();
    infoGap2->setWidth(16);
    infoGap2->setHeight(1);
    info->addView(infoGap2);

    ActionTile* aboutTile = new ActionTile("cloud/profile/about"_i18n, [] {
        brls::Application::pushActivity(
            new InfoPage(InfoPage::Section::About));
    });
    aboutTile->setWidth(0);
    aboutTile->setGrow(1);
    info->addView(aboutTile);

    this->addView(info);

    // --- Footer -------------------------------------------------------------
    brls::Box* footerSpacer = new brls::Box();
    footerSpacer->setGrow(1);
    footerSpacer->setHeight(1);
    this->addView(footerSpacer);

    brls::Label* footer = new brls::Label();
    footer->setFontSize(13);
    footer->setTextColor(theme["cloud/text_secondary"]);
    footer->setHeight(18);
    footer->setText("cloud/profile/version"_i18n + " " NXKIT_APP_VERSION);
    this->addView(footer);
}

void ProfilePage::willAppear(bool resetState) {
    brls::Box::willAppear(resetState);
    this->refreshAccount();
}

void ProfilePage::refreshAccount() {
    auto provider = DriveManager::instance().current();
    if (!provider) return;

    this->loggedIn = provider->isLoggedIn();
    this->applyAccount();

    if (!this->loggedIn) return;

    // Ignore taps while a request is already in flight.
    if (this->refreshing) return;
    this->refreshing = true;
    const uint64_t request = ++this->accountRequest;
    this->refreshTile->setTitle("cloud/profile/refreshing"_i18n);

    // A provider whose request never completes must not leave the card stuck on
    // "refreshing" forever: a stalled request has no reply to reset it.
    brls::delay(25000, [this, alive = this->alive, request] {
        if (!*alive || this->accountRequest != request) return;
        this->refreshing = false;
        this->refreshTile->setTitle("cloud/profile/refresh"_i18n);
    });

    provider->accountInfo(
        [this, alive = this->alive, request](const AccountInfo& info,
                                             const std::string& error) {
            brls::sync([this, alive, request, info, error] {
                if (!*alive || this->accountRequest != request) return;
                this->refreshing = false;
                this->refreshTile->setTitle("cloud/profile/refresh"_i18n);
                if (error.empty()) {
                    this->account = info;
                } else if (isAuthError(error)) {
                    // The saved cookie/token is no longer accepted: drop back
                    // to the signed-out card and point at the login action.
                    this->loggedIn = false;
                    this->account = AccountInfo();
                    brls::Application::notify(
                        "cloud/login/session_expired"_i18n);
                } else {
                    brls::Application::notify(error);
                }
                this->applyAccount();
            });
        });
}

void ProfilePage::applyAccount() {
    auto& manager = DriveManager::instance();
    auto provider = manager.current();
    if (!provider) return;

    brls::Theme theme = brls::Application::getTheme();

    bool multipleProviders = manager.providers().size() > 1;
    this->switchButton->setVisibility(multipleProviders
                                          ? brls::Visibility::VISIBLE
                                          : brls::Visibility::GONE);

    if (!this->loggedIn) {
        this->avatarLabel->setText("?");
        this->loadAvatar(std::string());
        this->nicknameLabel->setText("cloud/profile/guest"_i18n);
        this->nicknameLabel->setTextColor(theme["cloud/text_secondary"]);
        this->memberLabel->setVisibility(brls::Visibility::GONE);
        this->capacityBox->setVisibility(brls::Visibility::GONE);
        this->refreshTile->setVisibility(brls::Visibility::GONE);
        this->accountTile->setTitle(provider->id() == "smb"
                                        ? "cloud/smb/configure"_i18n
                                        : "cloud/profile/login"_i18n);
        return;
    }

    std::string nickname = this->account.nickname.empty()
                               ? "cloud/profile/unknown"_i18n
                               : this->account.nickname;
    this->avatarLabel->setText(firstCharacter(nickname));
    this->loadAvatar(this->account.avatarUrl);
    this->nicknameLabel->setText(nickname);
    this->nicknameLabel->setTextColor(theme["cloud/text"]);

    if (this->account.memberType.empty()) {
        this->memberLabel->setVisibility(brls::Visibility::GONE);
    } else {
        this->memberLabel->setText(this->account.memberType);
        this->memberLabel->setVisibility(brls::Visibility::VISIBLE);
    }

    if (this->account.totalCapacity > 0) {
        double used = (double)this->account.usedCapacity;
        double total = (double)this->account.totalCapacity;
        this->capacityValue->setText(formatBytes(this->account.usedCapacity) +
                                     " / " + formatBytes(this->account.totalCapacity));
        this->capacityBar->setProgress((float)(used / total));
        this->capacityBox->setVisibility(brls::Visibility::VISIBLE);
    } else {
        this->capacityBox->setVisibility(brls::Visibility::GONE);
    }

    this->refreshTile->setVisibility(brls::Visibility::VISIBLE);
    this->accountTile->setTitle("cloud/profile/logout"_i18n);
}

void ProfilePage::loadAvatar(const std::string& rawUrl) {
    std::string url = rawUrl;
    if (url.rfind("//", 0) == 0) url = "https:" + url;
    if (url == this->avatarUrl) return;
    this->avatarUrl = url;
    if (this->avatarView) this->avatarView->clear();
    if (url.empty()) return;

    cpr::async([this, alive = this->alive, url] {
        nxkit::HttpClient client;
        client.setTimeout(15000);
        nxkit::HttpResult result = client.get(url);
        brls::sync([this, alive, url, ok = result.ok,
                    body = result.body] {
            if (!*alive || url != this->avatarUrl) return;
            if (!ok || body.empty() || !this->avatarView) return;
            this->avatarView->setImageData(body);
        });
    });
}

void ProfilePage::openLogin() {
    brls::Application::pushActivity(new LoginPage());
}

void ProfilePage::logout() {
    auto provider = DriveManager::instance().current();
    if (!provider) return;

    // The SMB share has no QR step: signing out means configuring the
    // connection again, so it must not talk about scanning a code.
    std::string confirm = provider->id() == "smb"
                              ? "cloud/smb/logout_confirm"_i18n
                              : "cloud/profile/logout_confirm"_i18n;
    brls::Dialog* dialog = new brls::Dialog(confirm);
    dialog->addButton("cloud/files/action/cancel"_i18n, []() {});
    dialog->addButton("cloud/profile/logout"_i18n, [this]() {
        auto current = DriveManager::instance().current();
        if (!current) return;
        current->logout();
        this->loggedIn = false;
        this->refreshing = false;
        this->account = AccountInfo();
        this->applyAccount();
        brls::Application::notify("cloud/profile/logged_out"_i18n);
    });
    dialog->setCancelable(true);
    dialog->open();
}

void showProviderPicker(std::function<void()> onChanged) {
    auto& manager = DriveManager::instance();
    if (manager.providers().size() <= 1) return;

    std::vector<std::string> names;
    int selected = 0;
    auto current = manager.current();
    for (size_t i = 0; i < manager.providers().size(); i++) {
        names.push_back(manager.providers()[i]->name());
        if (current && manager.providers()[i]->id() == current->id())
            selected = (int)i;
    }

    brls::Dropdown* dropdown = new brls::Dropdown(
        "cloud/profile/provider"_i18n, names,
        [onChanged](int choice) {
            auto& mgr = DriveManager::instance();
            if (choice < 0 || choice >= (int)mgr.providers().size()) return;
            mgr.setCurrent(mgr.providers()[choice]->id());
            refreshProviderBadge();
            if (onChanged) onChanged();
        },
        selected);
    brls::Application::pushActivity(new brls::Activity(dropdown));
}

void ProfilePage::openProviderMenu() {
    showProviderPicker([this, alive = this->alive] {
        if (!*alive) return;
        this->account = AccountInfo();
        this->refreshAccount();
    });
}

void ProfilePage::reload() {
    this->account = AccountInfo();
    this->refreshing = false;
    this->refreshAccount();
}

void ProfilePage::checkUpdate() {
    if (this->checkingUpdate) return;
    this->checkingUpdate = true;
    if (this->updateTile)
        this->updateTile->setTitle("cloud/profile/checking"_i18n);

    checkForUpdate([this, alive = this->alive](bool ok, const UpdateInfo& info,
                                               const std::string& error) {
        brls::sync([this, alive, ok, info, error] {
            if (!*alive) return;
            this->checkingUpdate = false;
            if (this->updateTile)
                this->updateTile->setTitle("cloud/profile/check_update"_i18n);
            presentUpdateResult(info, ok, error, true);
        });
    });
}

void ProfilePage::installDesktopIcon() {
#ifdef BUILTIN_NSP
    brls::Dialog* dialog =
        new brls::Dialog("cloud/profile/desktop_warn"_i18n);
    dialog->addButton("cloud/files/action/cancel"_i18n, []() {});
    dialog->addButton("cloud/profile/ok"_i18n, []() {
        brls::Application::blockInputs();
        mini::InstallSD("romfs:/nsp_forwarder.nsp");
        appletRequestLaunchApplication(kForwarderTitleId, NULL);
    });
    dialog->open();
#endif
}

}  // namespace cloud
