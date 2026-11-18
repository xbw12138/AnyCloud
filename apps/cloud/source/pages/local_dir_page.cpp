//
// cloud: local directory picker used by "download to..." actions.
//

#include "cloud/pages/local_dir_page.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

using namespace brls::literals;

namespace cloud {

namespace {

constexpr int kMaxEntries = 400;

std::string defaultRoot() {
#ifdef __SWITCH__
    return "sdmc:/";
#else
    const char* home = std::getenv("HOME");
    return home && *home ? std::string(home) : std::string("/");
#endif
}

std::string normalize(const std::string& path) {
    if (path.empty()) return defaultRoot();
    std::string result = path;
    while (result.size() > 1 && result.back() == '/') result.pop_back();
#ifdef __SWITCH__
    // libnx expects "sdmc:/", not "sdmc:".
    if (result == "sdmc:") result = "sdmc:/";
#endif
    return result;
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

bool isRoot(const std::string& path) {
#ifdef __SWITCH__
    return path == "sdmc:" || path == "sdmc:/";
#else
    return path == "/";
#endif
}

std::string parentOf(const std::string& path) {
    if (isRoot(path)) return path;
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return defaultRoot();
    if (slash == 0) return "/";
#ifdef __SWITCH__
    if (path.compare(0, 6, "sdmc:/") == 0 && slash == 5) return "sdmc:/";
#endif
    return path.substr(0, slash);
}

bool isDirectory(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

NVGcolor withAlpha(NVGcolor color, float alpha) {
    return nvgRGBAf(color.r, color.g, color.b, alpha);
}

/// Small folder outline drawn with NanoVG; used as the row leading icon so
/// the list reads at a glance without depending on an icon font.
class FolderGlyph : public brls::View {
public:
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (width <= 0 || height <= 0) return;
        NVGcolor color = brls::Application::getTheme()["brls/accent"];
        float s = std::min(width, height);
        float cx = x + width * 0.5f;
        float cy = y + height * 0.5f;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, cx - 0.36f * s, cy - 0.20f * s, 0.28f * s,
                       0.13f * s, 0.04f * s);
        nvgRoundedRect(vg, cx - 0.36f * s, cy - 0.12f * s, 0.72f * s,
                       0.34f * s, 0.07f * s);
        nvgFillColor(vg, this->a(color));
        nvgFill(vg);
    }
};

/// Flat, focusable list row: folder icon, name, optional detail and chevron.
class DirRow : public brls::Box {
public:
    DirRow(const std::string& title, const std::string& detail,
           bool withIcon, std::function<void()> callback)
        : callback(std::move(callback)) {
        brls::Theme theme = brls::Application::getTheme();

        this->setAxis(brls::Axis::ROW);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setHeight(detail.empty() ? 56 : 62);
        this->setPadding(0, 18, 0, 18);
        this->setMarginBottom(8);
        this->setCornerRadius(16);
        this->setHighlightCornerRadius(16);
        this->setBorderColor(theme["brls/accent"]);
        this->setBorderThickness(0);
        this->setFocusable(true);

        if (withIcon) {
            FolderGlyph* glyph = new FolderGlyph();
            glyph->setDimensions(22, 22);
            glyph->setMarginRight(14);
            this->addView(glyph);
        }

        brls::Box* text = new brls::Box(brls::Axis::COLUMN);
        text->setGrow(1);
        text->setJustifyContent(brls::JustifyContent::CENTER);

        brls::Label* name = new brls::Label();
        name->setText(title);
        name->setFontSize(17);
        name->setTextColor(theme["cloud/text"]);
        name->setHeight(22);
        name->setSingleLine(true);
        text->addView(name);

        if (!detail.empty()) {
            brls::Label* sub = new brls::Label();
            sub->setText(detail);
            sub->setFontSize(13);
            sub->setTextColor(theme["cloud/text_secondary"]);
            sub->setHeight(17);
            sub->setMarginTop(3);
            sub->setSingleLine(true);
            text->addView(sub);
        }
        this->addView(text);

        brls::Label* chevron = new brls::Label();
        chevron->setText("›");
        chevron->setFontSize(22);
        chevron->setTextColor(theme["cloud/text_secondary"]);
        chevron->setHeight(24);
        this->addView(chevron);

        this->registerAction("cloud/download/enter"_i18n, brls::BUTTON_A,
                             [this](brls::View*) {
                                 if (this->callback) this->callback();
                                 return true;
                             });
        this->refreshStyle();
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

    std::function<void()> callback;
};

/// Pinned confirmation row at the bottom of the picker.
class ConfirmRow : public brls::Box {
public:
    explicit ConfirmRow(std::function<void()> callback)
        : callback(std::move(callback)) {
        brls::Theme theme = brls::Application::getTheme();

        this->setAxis(brls::Axis::ROW);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setHeight(60);
        this->setPadding(0, 22, 0, 22);
        this->setCornerRadius(18);
        this->setHighlightCornerRadius(18);
        this->setBorderColor(theme["brls/accent"]);
        this->setBorderThickness(0);
        this->setFocusable(true);

        brls::Label* title = new brls::Label();
        title->setText("cloud/download/use_this"_i18n);
        title->setFontSize(18);
        title->setTextColor(theme["cloud/text"]);
        title->setHeight(24);
        title->setGrow(1);
        this->addView(title);

        brls::Label* hint = new brls::Label();
        hint->setText("A / Y");
        hint->setFontSize(13);
        hint->setTextColor(theme["cloud/text_secondary"]);
        hint->setHeight(18);
        this->addView(hint);

        this->registerAction("cloud/download/use_this"_i18n, brls::BUTTON_A,
                             [this](brls::View*) {
                                 if (this->callback) this->callback();
                                 return true;
                             });
        this->refreshStyle();
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
        this->setBackgroundColor(withAlpha(theme["brls/accent"],
                                           this->isFocused() ? 0.28f : 0.16f));
        this->setBorderThickness(this->isFocused() ? 2.0f : 0.0f);
    }

    std::function<void()> callback;
};

}  // namespace

brls::View* LocalDirPage::createContentView() {
    brls::Theme theme = brls::Application::getTheme();

    brls::Box* root = new brls::Box(brls::Axis::COLUMN);
    root->setGrow(1);
    root->setPadding(24, 48, 20, 48);
    root->setBackgroundColor(theme["cloud/bg"]);

    brls::Label* title = new brls::Label();
    title->setText("cloud/download/choose_dir"_i18n);
    title->setFontSize(26);
    title->setTextColor(theme["cloud/text"]);
    title->setMarginBottom(12);
    root->addView(title);

    // Current path card: small caption plus the path itself.
    brls::Box* pathCard = new brls::Box(brls::Axis::COLUMN);
    pathCard->setPadding(12, 18, 12, 18);
    pathCard->setMarginBottom(14);
    pathCard->setCornerRadius(14);
    pathCard->setBackgroundColor(theme["cloud/surface"]);

    brls::Label* caption = new brls::Label();
    caption->setText("cloud/download/current_dir"_i18n);
    caption->setFontSize(13);
    caption->setTextColor(theme["cloud/text_secondary"]);
    caption->setHeight(17);
    pathCard->addView(caption);

    this->pathLabel = new brls::Label();
    this->pathLabel->setFontSize(16);
    this->pathLabel->setTextColor(theme["cloud/text"]);
    this->pathLabel->setMarginTop(4);
    pathCard->addView(this->pathLabel);
    root->addView(pathCard);

    brls::ScrollingFrame* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1);
    this->listBox = new brls::Box(brls::Axis::COLUMN);
    this->listBox->setGrow(1);
    scroll->setContentView(this->listBox);
    root->addView(scroll);

    this->registerAction("cloud/download/use_this"_i18n, brls::BUTTON_Y,
                         [this](brls::View*) {
                             this->confirm();
                             return true;
                         });
    this->registerAction("cloud/download/new_dir"_i18n, brls::BUTTON_X,
                         [this](brls::View*) {
                             this->promptNewDir();
                             return true;
                         });

    std::string start = normalize(this->startDirectory);
    if (!isDirectory(start)) start = defaultRoot();
    this->load(start);
    return root;
}

void LocalDirPage::load(const std::string& rawPath) {
    brls::Theme theme = brls::Application::getTheme();
    std::string path = normalize(rawPath);
    this->currentDirectory = path;
    this->pathLabel->setText(path);

    this->listBox->clearViews();
    brls::View* firstRow = nullptr;

    if (!isRoot(path)) {
        std::string parent = parentOf(path);
        DirRow* up = new DirRow("..", parent, false, [this, parent] {
            this->load(parent);
        });
        this->listBox->addView(up);
        firstRow = up;
    }

    std::vector<std::string> dirs;
    if (DIR* handle = opendir(path.c_str())) {
        while (dirent* entry = readdir(handle)) {
            std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            std::string full = joinPath(path, name);
            bool directory = entry->d_type == DT_DIR;
            if (entry->d_type == DT_UNKNOWN || entry->d_type == DT_LNK)
                directory = isDirectory(full);
            if (directory) dirs.push_back(name);
        }
        closedir(handle);
    }
    std::sort(dirs.begin(), dirs.end(),
              [](const std::string& a, const std::string& b) {
                  return strcasecmp(a.c_str(), b.c_str()) < 0;
              });

    size_t count = std::min(dirs.size(), (size_t)kMaxEntries);
    for (size_t i = 0; i < count; i++) {
        std::string full = joinPath(path, dirs[i]);
        DirRow* row = new DirRow(dirs[i], std::string(), true,
                                 [this, full] { this->load(full); });
        this->listBox->addView(row);
        if (!firstRow) firstRow = row;
    }

    if (dirs.empty()) {
        brls::Label* empty = new brls::Label();
        empty->setText("cloud/download/dir_empty"_i18n);
        empty->setFontSize(16);
        empty->setTextColor(theme["cloud/text_secondary"]);
        empty->setMarginTop(12);
        empty->setMarginLeft(4);
        this->listBox->addView(empty);
    }

    ConfirmRow* confirm = new ConfirmRow([this] { this->confirm(); });
    confirm->setMarginTop(10);
    this->listBox->addView(confirm);
    if (!firstRow) firstRow = confirm;

    brls::Application::giveFocus(firstRow);
}

void LocalDirPage::promptNewDir() {
    std::string current = this->currentDirectory;
    brls::Application::getImeManager()->openForText(
        [this, current](std::string name) {
            if (name.empty()) return;
            for (char& c : name) {
                if (c == '/' || c == '\\' || c == ':') c = '_';
            }
            std::string full = joinPath(current, name);
            if (mkdir(full.c_str(), 0777) != 0 && !isDirectory(full)) {
                brls::Application::notify(
                    "cloud/download/new_dir_failed"_i18n);
                return;
            }
            // Enter the freshly created folder so it is one tap away from
            // being confirmed as the download target.
            this->load(full);
        },
        "cloud/download/new_dir"_i18n, "", 64);
}

void LocalDirPage::confirm() {
    if (this->currentDirectory.empty()) return;
    auto callback = this->onPick;
    std::string directory = this->currentDirectory;
    if (callback) callback(directory);
    brls::Application::popActivity();
}

}  // namespace cloud
