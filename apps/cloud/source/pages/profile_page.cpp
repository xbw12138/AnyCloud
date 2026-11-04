//
// cloud: account card + provider actions page.
//

#include "cloud/pages/profile_page.hpp"

#include <algorithm>

#include "cloud/drive/drive_manager.hpp"
#include "cloud/pages/login_page.hpp"

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

ProfilePage::ProfilePage() {
    brls::Theme theme = brls::Application::getTheme();
    this->setAxis(brls::Axis::COLUMN);
    this->setPadding(24, 48, 24, 48);
    this->setBackgroundColor(theme["cloud/bg"]);
    this->build();
    this->refreshAccount();
}

void ProfilePage::build() {
    brls::Theme theme = brls::Application::getTheme();

    // --- Title + provider pill ----------------------------------------------
    brls::Box* titleRow = new brls::Box(brls::Axis::ROW);
    titleRow->setAlignItems(brls::AlignItems::CENTER);
    titleRow->setHeight(44);

    brls::Label* title = new brls::Label();
    title->setText("cloud/tab/profile"_i18n);
    title->setFontSize(32);
    title->setTextColor(theme["cloud/text"]);
    title->setHeight(40);
    titleRow->addView(title);

    brls::Box* spacer = new brls::Box();
    spacer->setGrow(1);
    spacer->setHeight(1);
    titleRow->addView(spacer);

    brls::Box* pill = new brls::Box(brls::Axis::ROW);
    pill->setAlignItems(brls::AlignItems::CENTER);
    pill->setHeight(34);
    pill->setPadding(0, 16, 0, 16);
    pill->setCornerRadius(17);
    pill->setBackgroundColor(theme["cloud/surface_alt"]);

    this->providerLabel = new brls::Label();
    this->providerLabel->setFontSize(14);
    this->providerLabel->setTextColor(theme["cloud/text_secondary"]);
    this->providerLabel->setHeight(18);
    pill->addView(this->providerLabel);
    titleRow->addView(pill);

    this->addView(titleRow);

    // --- Account card -------------------------------------------------------
    brls::Box* card = new brls::Box(brls::Axis::COLUMN);
    card->setMarginTop(18);
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
    this->accountTile->setGrow(1);
    actions->addView(this->accountTile);

    this->providerTile =
        new ActionTile("cloud/profile/switch"_i18n, [this] { this->openProviderMenu(); });
    this->providerTile->setGrow(1);
    this->providerTile->setVisibility(brls::Visibility::GONE);
    actions->addView(this->providerTile);

    this->addView(actions);

    // --- Footer -------------------------------------------------------------
    brls::Box* footerSpacer = new brls::Box();
    footerSpacer->setGrow(1);
    footerSpacer->setHeight(1);
    this->addView(footerSpacer);

    brls::Label* footer = new brls::Label();
    footer->setFontSize(13);
    footer->setTextColor(theme["cloud/text_secondary"]);
    footer->setHeight(18);
    footer->setText("cloud/profile/version"_i18n + " 1.0.0");
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

    provider->accountInfo(
        [this, alive = this->alive](const AccountInfo& info,
                                    const std::string& error) {
            brls::sync([this, alive, info, error] {
                if (!*alive) return;
                if (error.empty()) this->account = info;
                this->applyAccount();
            });
        });
}

void ProfilePage::applyAccount() {
    auto& manager = DriveManager::instance();
    auto provider = manager.current();
    if (!provider) return;

    brls::Theme theme = brls::Application::getTheme();

    this->providerLabel->setText(provider->name());

    bool multipleProviders = manager.providers().size() > 1;
    this->providerTile->setVisibility(multipleProviders ? brls::Visibility::VISIBLE
                                                        : brls::Visibility::GONE);

    if (!this->loggedIn) {
        this->avatarLabel->setText("?");
        this->nicknameLabel->setText("cloud/profile/guest"_i18n);
        this->nicknameLabel->setTextColor(theme["cloud/text_secondary"]);
        this->memberLabel->setVisibility(brls::Visibility::GONE);
        this->capacityBox->setVisibility(brls::Visibility::GONE);
        this->refreshTile->setVisibility(brls::Visibility::GONE);
        this->accountTile->setTitle("cloud/profile/login"_i18n);
        return;
    }

    std::string nickname = this->account.nickname.empty()
                               ? "cloud/profile/unknown"_i18n
                               : this->account.nickname;
    this->avatarLabel->setText(firstCharacter(nickname));
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

void ProfilePage::openLogin() {
    brls::Application::pushActivity(new LoginPage());
}

void ProfilePage::logout() {
    auto provider = DriveManager::instance().current();
    if (!provider) return;

    provider->logout();
    this->loggedIn = false;
    this->account = AccountInfo();
    this->applyAccount();
    brls::Application::notify("cloud/profile/logged_out"_i18n);
}

void ProfilePage::openProviderMenu() {
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
        [this](int choice) {
            auto& mgr = DriveManager::instance();
            if (choice < 0 || choice >= (int)mgr.providers().size()) return;
            mgr.setCurrent(mgr.providers()[choice]->id());
            this->account = AccountInfo();
            this->refreshAccount();
        },
        selected);
    brls::Application::pushActivity(new brls::Activity(dropdown));
}

}  // namespace cloud
