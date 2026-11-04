//
// cloud: file browser page (list, navigate, file operations).
//

#include "cloud/pages/file_list_page.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>

#include "cloud/drive/drive_manager.hpp"
#include "cloud/drive/recent_store.hpp"
#include "cloud/pages/image_viewer.hpp"
#include "cloud/pages/login_page.hpp"
#include "cloud/pages/player_page.hpp"
#include "cloud/ui/cloud_empty_view.hpp"
#include "nxkit/ui/recycling_grid.hpp"

using namespace brls::literals;

namespace cloud {

static void listLog(const std::string& line) {
#ifdef __SWITCH__
    FILE* file = fopen("sdmc:/switch/nxkit/qr_debug.log", "a");
#else
    FILE* file = fopen("/tmp/qr_debug.log", "a");
#endif
    if (!file) return;
    fprintf(file, "%s\n", line.c_str());
    fclose(file);
}

static const int kPageSize = 50;
// Tile height: 96px cover band plus two lines of text.
static const float kGridRowHeight = 168;
// Single column row height.
static const float kListRowHeight  = 96;

static std::string lowerName(const std::string& name) {
    std::string value = name;
    std::transform(value.begin(), value.end(), value.begin(), ::tolower);
    return value;
}

static std::string formatSize(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = (double)bytes;
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        unit++;
    }
    char buf[64];
    if (unit == 0)
        snprintf(buf, sizeof(buf), "%d B", (int)bytes);
    else
        snprintf(buf, sizeof(buf), "%.1f %s", value, units[unit]);
    return buf;
}

static std::string formatDate(int64_t ms) {
    if (ms <= 0) return "";
    time_t sec = (time_t)(ms / 1000);
    struct tm tmv {};
    localtime_r(&sec, &tmv);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
    return buf;
}

static std::string typeTagOf(const DriveFile& file) {
    if (file.isDir) return "cloud/files/type/dir"_i18n;
    switch (file.mediaKind()) {
        case MediaKind::Video: return "cloud/files/type/video"_i18n;
        case MediaKind::Audio: return "cloud/files/type/audio"_i18n;
        case MediaKind::Image: return "cloud/files/type/image"_i18n;
        default: return "cloud/files/type/other"_i18n;
    }
}

// -----------------------------------------------------------------------------
// Row visuals
// -----------------------------------------------------------------------------

static NVGcolor typeColor(bool isDir, MediaKind kind) {
    if (isDir) return nvgRGB(0, 166, 118);
    switch (kind) {
        case MediaKind::Video: return nvgRGB(255, 132, 76);
        case MediaKind::Audio: return nvgRGB(150, 120, 255);
        case MediaKind::Image: return nvgRGB(64, 150, 255);
        default: return nvgRGB(140, 150, 162);
    }
}

/// Washed-out type colour used behind the badge in grid cards.
static NVGcolor typeTint(bool isDir, MediaKind kind) {
    NVGcolor color = typeColor(isDir, kind);
    return nvgRGBAf(color.r, color.g, color.b, 0.16f);
}

/// Solid rounded square with a white glyph for the file type.
class FileTypeBadge : public brls::View {
public:
    void setKind(bool dir, MediaKind value) {
        this->isDir = dir;
        this->kind = value;
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (width <= 0 || height <= 0) return;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 12);
        nvgFillColor(vg, this->a(typeColor(this->isDir, this->kind)));
        nvgFill(vg);

        NVGcolor glyph = this->RGBA(255, 255, 255, 235);
        float cx = x + width * 0.5f;
        float cy = y + height * 0.5f;

        if (this->isDir) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, cx - 9, cy - 4, 18, 12, 2.5f);
            nvgStrokeColor(vg, glyph);
            nvgStrokeWidth(vg, 1.8f);
            nvgStroke(vg);

            nvgBeginPath(vg);
            nvgRoundedRect(vg, cx - 9, cy - 9, 9, 6, 1.5f);
            nvgFillColor(vg, glyph);
            nvgFill(vg);
            return;
        }

        switch (this->kind) {
            case MediaKind::Video:
                nvgBeginPath(vg);
                nvgMoveTo(vg, cx - 5, cy - 8);
                nvgLineTo(vg, cx - 5, cy + 8);
                nvgLineTo(vg, cx + 8, cy);
                nvgClosePath(vg);
                nvgFillColor(vg, glyph);
                nvgFill(vg);
                break;

            case MediaKind::Audio:
                nvgBeginPath(vg);
                nvgCircle(vg, cx - 3, cy + 6, 3.6f);
                nvgFillColor(vg, glyph);
                nvgFill(vg);
                nvgBeginPath(vg);
                nvgRect(vg, cx - 0.5f, cy - 8, 2.2f, 14);
                nvgFillColor(vg, glyph);
                nvgFill(vg);
                break;

            case MediaKind::Image:
                nvgBeginPath(vg);
                nvgRoundedRect(vg, cx - 9, cy - 8, 18, 16, 2.5f);
                nvgStrokeColor(vg, glyph);
                nvgStrokeWidth(vg, 1.8f);
                nvgStroke(vg);
                nvgBeginPath(vg);
                nvgCircle(vg, cx - 4, cy - 3, 1.8f);
                nvgFillColor(vg, glyph);
                nvgFill(vg);
                nvgBeginPath(vg);
                nvgMoveTo(vg, cx - 8, cy + 6);
                nvgLineTo(vg, cx - 1, cy);
                nvgLineTo(vg, cx + 8, cy + 6);
                nvgStrokeColor(vg, glyph);
                nvgStrokeWidth(vg, 1.8f);
                nvgStroke(vg);
                break;

            default:
                nvgBeginPath(vg);
                nvgRoundedRect(vg, cx - 7, cy - 9, 14, 18, 2.5f);
                nvgStrokeColor(vg, glyph);
                nvgStrokeWidth(vg, 1.8f);
                nvgStroke(vg);
                nvgBeginPath(vg);
                nvgMoveTo(vg, cx - 3, cy - 2);
                nvgLineTo(vg, cx + 3, cy - 2);
                nvgMoveTo(vg, cx - 3, cy + 2);
                nvgLineTo(vg, cx + 3, cy + 2);
                nvgStrokeColor(vg, glyph);
                nvgStrokeWidth(vg, 1.5f);
                nvgStroke(vg);
                break;
        }
    }

private:
    bool isDir = false;
    MediaKind kind = MediaKind::None;
};

/// Small accent chevron shown at the end of directory rows.
class ChevronView : public brls::View {
public:
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (width <= 0 || height <= 0) return;
        float cx = x + width * 0.5f;
        float cy = y + height * 0.5f;
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx - 4, cy - 8);
        nvgLineTo(vg, cx + 4, cy);
        nvgLineTo(vg, cx - 4, cy + 8);
        nvgStrokeColor(vg, this->a(brls::Application::getTheme()["brls/accent"]));
        nvgStrokeWidth(vg, 2.2f);
        nvgStroke(vg);
    }
};

/// Cover band of the grid card. It paints a soft tint of the file type with
/// rounded top corners matching the card, and hosts the type badge.
class CoverPlate : public brls::Box {
public:
    CoverPlate() : brls::Box(brls::Axis::ROW) {
        this->setJustifyContent(brls::JustifyContent::CENTER);
        this->setAlignItems(brls::AlignItems::CENTER);
    }

    void setTint(NVGcolor color) {
        this->tint = color;
        this->invalidate();
    }

    /// Hidden in list mode, where this view is only an icon slot.
    void setPlateVisible(bool value) {
        this->plateVisible = value;
        this->invalidate();
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (this->plateVisible && width > 0 && height > 0) {
            float radius = this->getCornerRadius();
            nvgBeginPath(vg);
            nvgRoundedRectVarying(vg, x, y, width, height, radius, radius, 0,
                                  0);
            nvgFillColor(vg, this->a(this->tint));
            nvgFill(vg);
        }
        brls::Box::draw(vg, x, y, width, height, style, ctx);
    }

private:
    NVGcolor tint = nvgRGBA(0, 0, 0, 0);
    bool plateVisible = false;
};

// -----------------------------------------------------------------------------
// FileRowCell
// -----------------------------------------------------------------------------

class FileRowCell : public RecyclingGridItem {
public:
    explicit FileRowCell(FileListPage* page)
        : page(page), listMode(page->isListMode()) {
        brls::Theme theme = brls::Application::getTheme();

        this->setFocusable(true);
        this->setBackgroundColor(theme["cloud/surface"]);
        this->setBorderThickness(0);

        // The thumb holds the type badge: a plain icon slot in list mode and
        // the tinted cover band of the grid card.
        this->thumb = new CoverPlate();
        this->badge = new FileTypeBadge();
        this->thumb->addView(this->badge);
        this->addView(this->thumb);

        this->textBox = new brls::Box(brls::Axis::COLUMN);
        this->textBox->setGrow(1);
        this->textBox->setJustifyContent(brls::JustifyContent::CENTER);
        this->addView(this->textBox);

        this->nameLabel = new brls::Label();
        this->nameLabel->setSingleLine(true);
        this->nameLabel->setAnimated(true);
        this->nameLabel->setTextColor(theme["cloud/text"]);
        this->textBox->addView(this->nameLabel);

        this->metaLabel = new brls::Label();
        this->metaLabel->setTextColor(theme["cloud/text_secondary"]);
        this->textBox->addView(this->metaLabel);

        this->chevron = new ChevronView();
        this->addView(this->chevron);

        this->applyLayout();

        // X opens the item actions menu. getIndex() resolves at press time,
        // so reused cells always act on the currently bound item.
        this->registerAction(
            "cloud/files/more"_i18n, brls::BUTTON_X,
            [this](brls::View*) -> bool {
                this->page->showItemActions(this->getIndex());
                return true;
            },
            false);
    }

    /// Switches between the compact grid card and the tall list row.
    void setListMode(bool value) {
        if (this->listMode == value) return;
        this->listMode = value;
        this->applyLayout();
    }

    void setFile(const DriveFile& file) {
        this->badge->setKind(file.isDir, file.mediaKind());
        this->thumb->setTint(typeTint(file.isDir, file.mediaKind()));
        this->nameLabel->setText(file.name);

        std::string meta = typeTagOf(file);
        if (!file.isDir && file.size > 0)
            meta += "  ·  " + formatSize(file.size);
        std::string date = formatDate(file.updatedAt);
        if (!date.empty()) meta += "  ·  " + date;
        this->metaLabel->setText(meta);

        this->chevron->setVisibility(this->listMode && file.isDir
                                         ? brls::Visibility::VISIBLE
                                         : brls::Visibility::GONE);
        this->refreshFocusStyle();
    }

    void onFocusGained() override {
        RecyclingGridItem::onFocusGained();
        this->refreshFocusStyle();
    }

    void onFocusLost() override {
        RecyclingGridItem::onFocusLost();
        this->refreshFocusStyle();
    }

    static RecyclingGridItem* create(FileListPage* page) {
        return new FileRowCell(page);
    }

private:
    void applyLayout() {
        if (this->listMode) {
            // Single column: icon, title and metadata on one line.
            this->setAxis(brls::Axis::ROW);
            this->setAlignItems(brls::AlignItems::CENTER);
            this->setPadding(0, 24, 0, 20);
            this->setCornerRadius(16);
            this->setHighlightCornerRadius(16);

            this->thumb->setPlateVisible(false);
            this->thumb->setCornerRadius(0);
            this->thumb->setDimensions(56, 56);
            this->thumb->setMarginRight(18);
            this->badge->setDimensions(56, 56);

            this->textBox->setGrow(1);
            this->textBox->setPadding(0);
            this->nameLabel->setFontSize(21);
            this->metaLabel->setFontSize(14);
            this->metaLabel->setMarginTop(4);

            this->chevron->setVisibility(brls::Visibility::VISIBLE);
            this->chevron->setDimensions(22, 22);
            this->chevron->setMarginLeft(12);
        } else {
            // Multi column tile: tinted cover band on top, text below.
            this->setAxis(brls::Axis::COLUMN);
            this->setAlignItems(brls::AlignItems::FLEX_START);
            this->setPadding(0);
            this->setCornerRadius(18);
            this->setHighlightCornerRadius(18);

            this->thumb->setPlateVisible(true);
            this->thumb->setCornerRadius(18);
            this->thumb->setWidthPercentage(100);
            this->thumb->setHeight(96);
            this->thumb->setMarginRight(0);
            this->badge->setDimensions(44, 44);

            this->textBox->setGrow(0);
            this->textBox->setPadding(12, 16, 14, 16);
            this->nameLabel->setFontSize(18);
            this->metaLabel->setFontSize(12);
            this->metaLabel->setMarginTop(3);

            this->chevron->setVisibility(brls::Visibility::GONE);
        }
        this->invalidate();
    }

    void refreshFocusStyle() {
        brls::Theme theme = brls::Application::getTheme();
        bool selected = this->isFocused();
        // Flat cards: only the focused one lifts to a lighter surface and
        // gains a thin accent outline.
        this->setBackgroundColor(selected ? theme["cloud/surface_alt"]
                                          : theme["cloud/surface"]);
        this->setBorderColor(theme["brls/accent"]);
        this->setBorderThickness(selected ? 2.0f : 0.0f);
        this->nameLabel->setTextColor(theme["cloud/text"]);
    }

    FileListPage* page;
    bool listMode = false;
    CoverPlate* thumb = nullptr;
    brls::Box* textBox = nullptr;
    FileTypeBadge* badge = nullptr;
    brls::Label* nameLabel = nullptr;
    brls::Label* metaLabel = nullptr;
    ChevronView* chevron = nullptr;
};

// -----------------------------------------------------------------------------
// FileListDataSource
// -----------------------------------------------------------------------------

class FileListDataSource : public RecyclingGridDataSource {
public:
    explicit FileListDataSource(FileListPage* page) : page(page) {}

    size_t getItemCount() override { return this->files.size(); }

    RecyclingGridItem* cellForRow(RecyclingGrid* recycler,
                                  size_t index) override {
        auto* cell = (FileRowCell*)recycler->dequeueReusableCell("file");
        cell->setListMode(this->page->isListMode());
        cell->setFile(this->files[index]);
        return cell;
    }

    float heightForRow(RecyclingGrid*, size_t) override {
        return this->page->isListMode() ? kListRowHeight : kGridRowHeight;
    }

    void onItemSelected(RecyclingGrid*, size_t index) override {
        this->page->openItem(index);
    }

    void clearData() override { this->files.clear(); }

    FileListPage* page;
    std::vector<DriveFile> files;
};

// -----------------------------------------------------------------------------
// FileListPage
// -----------------------------------------------------------------------------

FileListPage::FileListPage() : mode(Mode::Browse) {
    this->provider = DriveManager::instance().current();
    this->pathStack.emplace_back(this->provider->rootId(), "");
    this->setup();
    this->refresh();
}

FileListPage::FileListPage(Mode mode) : mode(mode) {
    this->provider = DriveManager::instance().current();
    this->pathStack.emplace_back(this->provider->rootId(), "");
    this->setup();
    this->refresh();
}

FileListPage::FileListPage(
    std::function<void(const std::string& dirId, const std::string& dirName)>
        onPick)
    : mode(Mode::Picker), pickCallback(std::move(onPick)) {
    this->provider = DriveManager::instance().current();
    this->pathStack.emplace_back(this->provider->rootId(), "");
    this->setup();
    this->refresh();
}

FileListPage::~FileListPage() { *this->alive = false; }

void FileListPage::willAppear(bool resetState) {
    brls::Box::willAppear(resetState);
    // Recent views are local, refresh them whenever the tab is shown again.
    if (this->mode == Mode::Recent) {
        this->refresh();
        return;
    }
    // The page is built before login, so reload once the account is connected.
    if (this->provider->isLoggedIn() != this->wasLoggedIn) this->refresh();
}

brls::View* FileListPage::getDefaultFocus() {
    if (this->emptyView &&
        this->emptyView->getVisibility() == brls::Visibility::VISIBLE) {
        if (auto* target = this->emptyView->focusTarget()) return target;
    }
    if (this->grid &&
        this->grid->getVisibility() == brls::Visibility::VISIBLE &&
        this->grid->getItemCount() > 0) {
        if (brls::View* target = this->grid->getDefaultFocus()) return target;
    }
    return brls::Box::getDefaultFocus();
}

void FileListPage::setup() {
    brls::Theme theme = brls::Application::getTheme();
    this->setAxis(brls::Axis::COLUMN);

    brls::Box* header = new brls::Box(brls::Axis::COLUMN);
    header->setPadding(16, 40, 12, 40);

    brls::Box* titleRow = new brls::Box(brls::Axis::ROW);
    titleRow->setAlignItems(brls::AlignItems::CENTER);

    this->breadcrumb = new brls::Label();
    this->breadcrumb->setGrow(1);
    this->breadcrumb->setFontSize(30);
    this->breadcrumb->setTextColor(theme["cloud/text"]);
    this->breadcrumb->setSingleLine(true);
    this->breadcrumb->setAnimated(true);
    titleRow->addView(this->breadcrumb);

    this->hintLabel = new brls::Label();
    this->hintLabel->setFontSize(12);
    this->hintLabel->setTextColor(theme["cloud/text_secondary"]);
    this->hintLabel->setText(this->mode == Mode::Picker
                                 ? "cloud/files/hint_picker"_i18n
                                 : "cloud/files/hint_browse"_i18n);
    this->hintLabel->setMarginRight(18);
    titleRow->addView(this->hintLabel);

    this->viewToggle = new brls::Button();
    this->viewToggle->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
    this->viewToggle->setFontSize(14);
    this->viewToggle->setText(this->listMode ? "cloud/files/view_grid"_i18n
                                            : "cloud/files/view_list"_i18n);
    this->viewToggle->setMarginLeft(12);
    this->viewToggle->registerClickAction([this](brls::View*) {
        this->toggleViewMode();
        return true;
    });
    titleRow->addView(this->viewToggle);

    // Y is taken by the sort menu now, so creating a folder needs a visible
    // entry point of its own (only meaningful in the real browser).
    if (this->mode == Mode::Browse) {
        brls::Button* newFolder = new brls::Button();
        newFolder->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
        newFolder->setFontSize(14);
        newFolder->setText("cloud/files/mkdir"_i18n);
        newFolder->setMarginLeft(10);
        newFolder->registerClickAction([this](brls::View*) {
            this->promptMakeDir();
            return true;
        });
        titleRow->addView(newFolder);
    }

    header->addView(titleRow);

    // Second line: the parent path on the left, the item count on the right.
    // The path keeps a flexible width so the count stays right-aligned even
    // when the breadcrumb is hidden at the root.
    brls::Box* metaRow = new brls::Box(brls::Axis::ROW);
    metaRow->setAlignItems(brls::AlignItems::CENTER);
    metaRow->setMarginTop(6);

    // Both labels get a fixed line height: an empty label measures as an
    // undefined height, which made the header swallow the whole page.
    this->pathLabel = new brls::Label();
    this->pathLabel->setFontSize(13);
    this->pathLabel->setTextColor(theme["cloud/text_secondary"]);
    this->pathLabel->setSingleLine(true);
    this->pathLabel->setAnimated(true);
    this->pathLabel->setHeight(18);
    metaRow->addView(this->pathLabel);

    // Pushes the item count to the right edge, whether or not there is a
    // parent path to show.
    brls::Box* metaSpacer = new brls::Box();
    metaSpacer->setGrow(1);
    metaSpacer->setHeight(18);
    metaRow->addView(metaSpacer);

    this->subtitleLabel = new brls::Label();
    this->subtitleLabel->setFontSize(13);
    this->subtitleLabel->setTextColor(theme["cloud/text_secondary"]);
    this->subtitleLabel->setMarginLeft(16);
    this->subtitleLabel->setHeight(18);
    metaRow->addView(this->subtitleLabel);

    header->addView(metaRow);

    this->addView(header);

    this->grid = new RecyclingGrid();
    this->grid->setGrow(1);
    // This page draws its own empty-state illustration; the grid's built-in
    // placeholder would flash while a folder is loading.
    this->grid->setHintEnabled(false);
    this->grid->spanCount          = this->listMode ? 1 : 4;
    this->grid->estimatedRowHeight =
        this->listMode ? kListRowHeight : kGridRowHeight;
    this->grid->estimatedRowSpace = this->listMode ? 8 : 14;
    this->grid->setPaddingTop(4);
    this->grid->setPaddingBottom(32);
    this->grid->setPaddingLeft(40);
    this->grid->setPaddingRight(40);

    auto* source = new FileListDataSource(this);
    this->dataSource = source;
    this->grid->registerCell("file", [this] { return FileRowCell::create(this); });
    this->grid->setDataSource(source);
    this->grid->onNextPage([this] { this->loadNextPage(); });

    this->addView(this->grid);

    this->emptyView = new CloudEmptyView();
    this->emptyView->setVisibility(brls::Visibility::GONE);
    this->addView(this->emptyView);

    // B is handled by the tab frame (see onBackRequested), which asks the page
    // first so folder navigation still works.

    if (this->mode == Mode::Picker) {
        // Y picks the current directory.
        this->registerAction(
            "cloud/files/pick_here"_i18n, brls::BUTTON_Y,
            [this](brls::View*) -> bool {
                this->pickCurrentDirectory();
                return true;
            });
    } else {
        // Y opens the sort menu.
        this->registerAction(
            "cloud/files/sort"_i18n, brls::BUTTON_Y,
            [this](brls::View*) -> bool {
                this->showSortMenu();
                return true;
            });
    }

    this->updateBreadcrumb();
}

void FileListPage::updateBreadcrumb() {
    if (this->mode == Mode::Recent) {
        this->breadcrumb->setText("cloud/tab/recent"_i18n);
        this->pathLabel->setText("");
        this->pathLabel->setVisibility(brls::Visibility::GONE);
        return;
    }

    bool picker = this->mode == Mode::Picker;
    size_t depth = this->pathStack.size();

    // Big title is the folder being browsed, not the whole trail.
    std::string title = depth > 1 ? this->pathStack.back().second
                                  : (picker ? "cloud/files/picker_title"_i18n
                                            : "cloud/files/root"_i18n);
    this->breadcrumb->setText(title);

    // Small line under it holds the parent trail (without the current
    // folder, which is already the title). At the root there is nothing to
    // show, so it stays empty and only the item count remains.
    std::string path;
    if (depth > 1) {
        path = picker ? "cloud/files/picker_title"_i18n
                      : "cloud/files/root"_i18n;
        for (size_t i = 1; i + 1 < depth; i++)
            path += " / " + this->pathStack[i].second;
    }
    this->pathLabel->setText(path);
    this->pathLabel->setVisibility(path.empty() ? brls::Visibility::GONE
                                                : brls::Visibility::VISIBLE);
    brls::Logger::info("cloud: breadcrumb '{}'", title);
}

void FileListPage::updateSubtitle() {
    auto* source = (FileListDataSource*)this->dataSource;
    size_t count = source->files.size();
    std::string text = std::to_string(count) + " " + "cloud/files/items"_i18n;
    if (this->total > 0 && (size_t)this->total != count)
        text = std::to_string(count) + " / " + std::to_string(this->total) +
               " " + "cloud/files/items"_i18n;
    this->subtitleLabel->setText(text);
}

bool FileListPage::isFocusWithin(brls::View* view, brls::View* ancestor) {
    for (brls::View* current = view; current; current = current->getParent())
        if (current == ancestor) return true;
    return false;
}

void FileListPage::showGrid() {
    this->emptyView->setVisibility(brls::Visibility::GONE);
    this->grid->setVisibility(brls::Visibility::VISIBLE);
}

void FileListPage::showLoginState() {
    this->grid->setVisibility(brls::Visibility::GONE);
    this->emptyView->setVisibility(brls::Visibility::VISIBLE);
    this->emptyView->setContent(CloudEmptyView::Mode::Login,
                                "cloud/files/login_title"_i18n,
                                "cloud/files/login_desc"_i18n);
    this->emptyView->setAction(
        "cloud/files/login_action"_i18n, [] {
            brls::Application::pushActivity(new LoginPage());
        });
    this->subtitleLabel->setText("cloud/files/not_signed_in"_i18n);

    // Move focus away from a list that is about to disappear.
    if (isFocusWithin(brls::Application::getCurrentFocus(), this->grid)) {
        if (auto* target = this->emptyView->focusTarget())
            brls::Application::giveFocus(target);
    }
}

void FileListPage::showEmptyState() {
    listLog("list: empty state total=" + std::to_string(this->total) +
            " dir=" + this->pathStack.back().second);
    this->grid->setVisibility(brls::Visibility::GONE);
    this->emptyView->setVisibility(brls::Visibility::VISIBLE);

    if (this->mode == Mode::Recent) {
        this->emptyView->setContent(CloudEmptyView::Mode::Empty,
                                    "cloud/recent/empty_title"_i18n,
                                    "cloud/recent/empty_desc"_i18n);
        this->emptyView->setAction("", nullptr);
    } else if (this->mode == Mode::Picker) {
        this->emptyView->setContent(
            CloudEmptyView::Mode::Empty,
            "cloud/files/empty_picker_title"_i18n,
            "cloud/files/empty_picker_desc"_i18n);
        this->emptyView->setAction("cloud/files/pick_here"_i18n,
                                   [this] { this->pickCurrentDirectory(); });
    } else {
        this->emptyView->setContent(CloudEmptyView::Mode::Empty,
                                    "cloud/files/empty_title"_i18n,
                                    "cloud/files/empty_desc"_i18n);
        this->emptyView->setAction("cloud/files/mkdir"_i18n,
                                   [this] { this->promptMakeDir(); });
    }
    this->updateSubtitle();

    if (isFocusWithin(brls::Application::getCurrentFocus(), this->grid)) {
        if (auto* target = this->emptyView->focusTarget())
            brls::Application::giveFocus(target);
    }
}

void FileListPage::showErrorState(const std::string& error) {
    listLog("list: error state: " + error);
    this->grid->setVisibility(brls::Visibility::GONE);
    this->emptyView->setVisibility(brls::Visibility::VISIBLE);
    this->emptyView->setContent(
        CloudEmptyView::Mode::Error, "cloud/files/error_title"_i18n,
        error.empty() ? "cloud/files/error_desc"_i18n : error);
    this->emptyView->setAction("cloud/files/retry"_i18n,
                               [this] { this->refresh(); });
    this->subtitleLabel->setText("");

    if (isFocusWithin(brls::Application::getCurrentFocus(), this->grid)) {
        if (auto* target = this->emptyView->focusTarget())
            brls::Application::giveFocus(target);
    }
}

void FileListPage::refresh() {
    if (this->mode == Mode::Recent) {
        this->refreshRecent();
        return;
    }

    bool loggedIn = this->provider->isLoggedIn();
    this->wasLoggedIn = loggedIn;

    auto* source = (FileListDataSource*)this->dataSource;
    source->files.clear();
    this->page = 0;
    this->total = -1;

    if (!loggedIn) {
        this->grid->reloadData();
        this->showLoginState();
        return;
    }

    // Invalidate any request still in flight; otherwise its stale guard leaves
    // `loading` stuck at true and the reload never starts.
    this->requestSeq += 1;
    this->loading = false;
    this->showGrid();
    this->grid->reloadData();
    this->subtitleLabel->setText("cloud/files/loading"_i18n);
    this->loadNextPage();
}

void FileListPage::refreshRecent() {
    this->requestSeq += 1;
    this->loading = false;
    this->page = 0;
    this->total = -1;

    auto* source = (FileListDataSource*)this->dataSource;
    source->files = RecentStore::instance().files();
    this->applySort();

    if (source->files.empty()) {
        this->grid->reloadData();
        this->showEmptyState();
        return;
    }

    this->showGrid();
    this->updateSubtitle();
    this->grid->reloadData();
}

void FileListPage::applySort() {
    auto* source = (FileListDataSource*)this->dataSource;
    if (this->sortMode == SortMode::Time) {
        // RecentStore already keeps its entries newest-first.
        if (this->mode == Mode::Recent) return;
        std::stable_sort(source->files.begin(), source->files.end(),
                         [](const DriveFile& a, const DriveFile& b) {
                             return a.updatedAt > b.updatedAt;
                         });
        return;
    }

    std::stable_sort(source->files.begin(), source->files.end(),
                     [](const DriveFile& a, const DriveFile& b) {
                         std::string left = lowerName(a.name);
                         std::string right = lowerName(b.name);
                         if (left != right) return left < right;
                         return a.updatedAt > b.updatedAt;
                     });
}

void FileListPage::showSortMenu() {
    std::vector<std::string> options = {
        "cloud/files/sort_time"_i18n,
        "cloud/files/sort_name"_i18n,
    };
    int selected = this->sortMode == SortMode::Name ? 1 : 0;
    brls::Dropdown* dropdown = new brls::Dropdown(
        "cloud/files/sort"_i18n, options,
        [this](int choice) {
            SortMode next = choice == 1 ? SortMode::Name : SortMode::Time;
            if (next == this->sortMode) return;
            this->sortMode = next;
            this->applySort();
            this->grid->reloadData();
        },
        selected);
    brls::Application::pushActivity(new brls::Activity(dropdown));
}

void FileListPage::toggleViewMode() {
    this->listMode = !this->listMode;
    if (this->viewToggle)
        this->viewToggle->setText(this->listMode
                                      ? "cloud/files/view_grid"_i18n
                                      : "cloud/files/view_list"_i18n);
    if (this->grid) {
        // List mode is a single column of wide rows, grid mode is a four
        // column wall of tiles.
        this->grid->spanCount          = this->listMode ? 1 : 4;
        this->grid->estimatedRowHeight =
            this->listMode ? kListRowHeight : kGridRowHeight;
        this->grid->estimatedRowSpace = this->listMode ? 8 : 14;
        this->grid->reloadData();
    }
}

void FileListPage::loadNextPage() {
    if (this->mode == Mode::Recent) return;
    if (this->loading) return;
    if (this->total >= 0 &&
        (int)((FileListDataSource*)this->dataSource)->files.size() >=
            this->total)
        return;
    this->loading = true;
    int nextPage = this->page + 1;
    std::string dirId = this->pathStack.back().first;
    uint64_t seq = this->requestSeq;

    this->provider->listFiles(
        dirId, nextPage, kPageSize,
        [this, alive = this->alive, dirId, seq](
            const std::vector<DriveFile>& files, int total,
            const std::string& error) {
            brls::sync([this, alive, dirId, seq, files, total, error] {
                if (!*alive) return;
                // Stale response after navigating away or reloading.
                if (seq != this->requestSeq) return;
                if (dirId != this->pathStack.back().first) return;
                this->loading = false;
                brls::Logger::info("cloud: {} got {} entries, total {}", dirId,
                                   files.size(), total);
                this->applyFiles(files, total, this->page == 0, error);
            });
        });
}

void FileListPage::applyFiles(const std::vector<DriveFile>& files, int total,
                              bool firstPage, const std::string& error) {
    auto* source = (FileListDataSource*)this->dataSource;
    if (!error.empty()) {
        if (firstPage)
            this->showErrorState(error);
        else
            brls::Application::notify(error);
        return;
    }

    this->page += 1;
    this->total = total;

    for (const auto& file : files) {
        if (this->mode == Mode::Picker && !file.isDir) continue;
        source->files.push_back(file);
    }

    this->applySort();

    // Only treat the folder as empty when the server says so: an empty page
    // for a non-empty folder would otherwise flash the empty state illustration
    // for one frame while the directory loads.
    if (source->files.empty() && this->total == 0) {
        this->showEmptyState();
        return;
    }

    // Remember whether focus sat inside the empty state so it can be handed
    // back to the list once the cells exist.
    bool focusWasEmpty = isFocusWithin(brls::Application::getCurrentFocus(),
                                       this->emptyView);
    this->showGrid();
    this->updateSubtitle();

    // The client-side sort can move freshly loaded pages into the middle of
    // the list, so existing cells have to be rebound. Keep the scroll position
    // for pagination to avoid jumping back to the top.
    float offset = this->grid->getContentOffsetY();
    this->grid->reloadData();
    if (!firstPage) {
        auto alive = this->alive;
        brls::delay(100, [this, alive, offset] {
            if (!*alive) return;
            this->grid->setContentOffsetY(offset, false);
        });
    }

    if (focusWasEmpty) {
        brls::Application::giveFocus(this->grid);
        // The first layout of a previously hidden grid happens after this
        // callback, so retry once if focus is still on the hidden button.
        auto alive = this->alive;
        brls::delay(150, [this, alive] {
            if (!*alive) return;
            if (isFocusWithin(brls::Application::getCurrentFocus(),
                              this->emptyView))
                brls::Application::giveFocus(this->grid);
        });
    }
}

void FileListPage::enterFolder(const std::string& dirId,
                               const std::string& name) {
    brls::Logger::info("cloud: enter folder '{}' ({})", name, dirId);
    listLog("list: enter '" + name + "' depth=" +
            std::to_string(this->pathStack.size() + 1));
    this->pathStack.emplace_back(dirId, name);
    this->updateBreadcrumb();
    this->refresh();
}

bool FileListPage::onBackRequested() { return this->goUp(); }

bool FileListPage::goUp() {
    if (this->pathStack.size() <= 1) return false;  // let the activity pop
    listLog("list: go up depth=" + std::to_string(this->pathStack.size()));
    this->pathStack.pop_back();
    this->updateBreadcrumb();
    this->refresh();
    return true;
}

void FileListPage::openItem(size_t index) {
    auto* source = (FileListDataSource*)this->dataSource;
    if (index >= source->files.size()) return;
    const DriveFile& file = source->files[index];

    if (file.isDir) {
        this->enterFolder(file.id, file.name);
        return;
    }
    if (this->mode == Mode::Picker) return;

    MediaKind kind = file.mediaKind();
    if (kind == MediaKind::Video || kind == MediaKind::Audio ||
        kind == MediaKind::Image)
        RecentStore::instance().add(file);

    switch (kind) {
        case MediaKind::Video:
            brls::Application::pushActivity(new brls::Activity(
                new PlayerPage(this->provider, file, false)));
            break;
        case MediaKind::Audio:
            brls::Application::pushActivity(new brls::Activity(
                new PlayerPage(this->provider, file, true)));
            break;
        case MediaKind::Image: {
            std::vector<DriveFile> images;
            size_t start = 0;
            for (const auto& f : source->files) {
                if (f.mediaKind() != MediaKind::Image) continue;
                if (f.id == file.id) start = images.size();
                images.push_back(f);
            }
            brls::Application::pushActivity(new brls::Activity(
                new ImageViewer(this->provider, std::move(images), start)));
            break;
        }
        default:
            brls::Application::notify("cloud/files/unsupported"_i18n);
            break;
    }
}

void FileListPage::showItemActions(size_t index) {
    if (this->mode == Mode::Picker) return;
    auto* source = (FileListDataSource*)this->dataSource;
    if (index >= source->files.size()) return;
    DriveFile file = source->files[index];

    std::vector<std::string> options = {
        "cloud/files/action/rename"_i18n,
        "cloud/files/action/delete"_i18n,
        "cloud/files/action/move"_i18n,
    };
    brls::Dropdown* dropdown = new brls::Dropdown(
        file.name, options, [this, file](int selected) {
            switch (selected) {
                case 0: this->promptRename(file); break;
                case 1: this->confirmDelete(file); break;
                case 2: this->pickMoveTarget(file); break;
                default: break;
            }
        });
    brls::Application::pushActivity(new brls::Activity(dropdown));
}

void FileListPage::promptMakeDir() {
    brls::Application::getImeManager()->openForText(
        [this](std::string name) {
            if (name.empty()) return;
            this->provider->makeDir(
                this->pathStack.back().first, name,
                [this, alive = this->alive, name](const std::string& error) {
                    brls::sync([this, alive, error, name] {
                        if (!*alive) return;
                        if (!error.empty()) {
                            brls::Application::notify(error);
                            return;
                        }
                        this->refresh();
                        // The listing can lag behind the create call, so check
                        // once more and reload if the new folder is missing.
                        auto alive = this->alive;
                        brls::delay(900, [this, alive, name] {
                            if (!*alive) return;
                            auto* source =
                                (FileListDataSource*)this->dataSource;
                            for (const auto& file : source->files) {
                                if (file.isDir && file.name == name) return;
                            }
                            listLog("list: mkdir retry, '" + name +
                                    "' missing from listing");
                            this->refresh();
                        });
                    });
                });
        },
        "cloud/files/mkdir"_i18n, "", 64);
}

void FileListPage::pickCurrentDirectory() {
    if (this->pickCallback)
        this->pickCallback(this->pathStack.back().first,
                           this->pathStack.back().second);
    brls::Application::popActivity();
}

void FileListPage::promptRename(const DriveFile& file) {
    brls::Application::getImeManager()->openForText(
        [this, file](std::string name) {
            if (name.empty() || name == file.name) return;
            this->provider->rename(
                file.id, name,
                [this, alive = this->alive](const std::string& error) {
                    brls::sync([this, alive, error] {
                        if (!*alive) return;
                        if (!error.empty())
                            brls::Application::notify(error);
                        else
                            this->refresh();
                    });
                });
        },
        "cloud/files/action/rename"_i18n, "", 255, file.name);
}

void FileListPage::confirmDelete(const DriveFile& file) {
    auto* dialog = new brls::Dialog(
        "cloud/files/delete_confirm"_i18n + ": " + file.name);
    dialog->addButton("cloud/files/action/cancel"_i18n, [] {});
    dialog->addButton("cloud/files/action/delete"_i18n, [this, file] {
        this->provider->remove(
            {file.id}, [this, alive = this->alive](const std::string& error) {
                brls::sync([this, alive, error] {
                    if (!*alive) return;
                    if (!error.empty())
                        brls::Application::notify(error);
                    else
                        this->refresh();
                });
            });
    });
    dialog->setCancelable(true);
    dialog->open();
}

void FileListPage::pickMoveTarget(const DriveFile& file) {
    auto* picker = new FileListPage(
        [this, file](const std::string& dirId, const std::string&) {
            this->provider->move(
                {file.id}, dirId,
                [this, alive = this->alive](const std::string& error) {
                    brls::sync([this, alive, error] {
                        if (!*alive) return;
                        if (!error.empty())
                            brls::Application::notify(error);
                        else
                            this->refresh();
                    });
                });
        });
    brls::Application::pushActivity(new brls::Activity(picker));
}

}  // namespace cloud
