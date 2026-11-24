//
// cloud: local file picker used by the upload action.
//

#include "cloud/pages/local_file_picker.hpp"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace brls::literals;

namespace cloud {

namespace {

constexpr int kMaxEntries = 600;

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
    if (result == "sdmc:") result = "sdmc:/";
#endif
    return result;
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

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string formatSize(uint64_t bytes) {
    char buffer[64];
    if (bytes >= 1024ULL * 1024ULL * 1024ULL)
        snprintf(buffer, sizeof(buffer), "%.2f GB",
                 (double)bytes / (1024.0 * 1024.0 * 1024.0));
    else if (bytes >= 1024ULL * 1024ULL)
        snprintf(buffer, sizeof(buffer), "%.1f MB",
                 (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024ULL)
        snprintf(buffer, sizeof(buffer), "%.1f KB", (double)bytes / 1024.0);
    else
        snprintf(buffer, sizeof(buffer), "%llu B",
                 (unsigned long long)bytes);
    return buffer;
}

/// Folder / document glyph drawn with NanoVG so the list does not depend on
/// an icon font.
class EntryGlyph : public brls::View {
public:
    explicit EntryGlyph(bool directory) : directory(directory) {}

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (width <= 0 || height <= 0) return;
        NVGcolor color = brls::Application::getTheme()["brls/accent"];
        float s = std::min(width, height);
        float cx = x + width * 0.5f;
        float cy = y + height * 0.5f;
        nvgBeginPath(vg);
        if (this->directory) {
            nvgRoundedRect(vg, cx - 0.36f * s, cy - 0.20f * s, 0.28f * s,
                           0.13f * s, 0.04f * s);
            nvgRoundedRect(vg, cx - 0.36f * s, cy - 0.12f * s, 0.72f * s,
                           0.34f * s, 0.07f * s);
        } else {
            nvgRoundedRect(vg, cx - 0.24f * s, cy - 0.34f * s, 0.48f * s,
                           0.68f * s, 0.06f * s);
        }
        nvgFillColor(vg, nvgRGBAf(color.r, color.g, color.b, 0.9f));
        nvgFill(vg);
    }

private:
    bool directory;
};

class EntryRow : public brls::Box {
public:
    EntryRow(const std::string& title, const std::string& detail,
             bool directory, std::function<void()> callback)
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

        EntryGlyph* glyph = new EntryGlyph(directory);
        glyph->setDimensions(22, 22);
        glyph->setMarginRight(14);
        this->addView(glyph);

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
        chevron->setText(directory ? "›" : "A");
        chevron->setFontSize(directory ? 22 : 13);
        chevron->setTextColor(theme["cloud/text_secondary"]);
        chevron->setHeight(24);
        this->addView(chevron);

        this->registerAction(
            directory ? "cloud/upload/enter"_i18n : "cloud/upload/select"_i18n,
            brls::BUTTON_A, [this](brls::View*) {
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

}  // namespace

brls::View* LocalFilePickerPage::createContentView() {
    brls::Theme theme = brls::Application::getTheme();

    brls::Box* root = new brls::Box(brls::Axis::COLUMN);
    root->setGrow(1);
    root->setPadding(24, 48, 20, 48);
    root->setBackgroundColor(theme["cloud/bg"]);

    brls::Label* title = new brls::Label();
    title->setText("cloud/upload/choose_file"_i18n);
    title->setFontSize(26);
    title->setTextColor(theme["cloud/text"]);
    title->setMarginBottom(12);
    root->addView(title);

    brls::Box* pathCard = new brls::Box(brls::Axis::COLUMN);
    pathCard->setPadding(12, 18, 12, 18);
    pathCard->setMarginBottom(14);
    pathCard->setCornerRadius(14);
    pathCard->setBackgroundColor(theme["cloud/surface"]);

    brls::Label* caption = new brls::Label();
    caption->setText("cloud/upload/current_dir"_i18n);
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

    // Register on the root view: Activity::registerAction is a no-op while the
    // content view is still being built, which left B doing nothing here.
    root->registerAction("cloud/upload/up"_i18n, brls::BUTTON_B,
                         [this](brls::View*) {
                             if (isRoot(this->currentDirectory)) {
                                 brls::Application::popActivity();
                             } else {
                                 this->load(parentOf(this->currentDirectory));
                             }
                             return true;
                         });

    std::string start = normalize(this->startDirectory);
    struct stat info;
    if (stat(start.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
        start = defaultRoot();
    this->load(start);
    return root;
}

void LocalFilePickerPage::load(const std::string& rawPath) {
    brls::Theme theme = brls::Application::getTheme();
    std::string path = normalize(rawPath);
    this->currentDirectory = path;
    this->pathLabel->setText(path);
    this->listBox->clearViews();

    brls::View* firstRow = nullptr;
    if (!isRoot(path)) {
        std::string parent = parentOf(path);
        EntryRow* up = new EntryRow("..", std::string(), true, [this, parent] {
            this->load(parent);
        });
        this->listBox->addView(up);
        firstRow = up;
    }

    std::vector<std::string> dirs;
    std::vector<std::pair<std::string, uint64_t>> files;
    if (DIR* handle = opendir(path.c_str())) {
        while (dirent* entry = readdir(handle)) {
            std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            std::string full = joinPath(path, name);
            bool directory = entry->d_type == DT_DIR;
            struct stat info;
            if (entry->d_type == DT_UNKNOWN || entry->d_type == DT_LNK)
                directory = stat(full.c_str(), &info) == 0 &&
                            S_ISDIR(info.st_mode);
            if (directory) {
                dirs.push_back(name);
            } else if (entry->d_type == DT_REG ||
                       (entry->d_type == DT_UNKNOWN &&
                        stat(full.c_str(), &info) == 0 &&
                        S_ISREG(info.st_mode))) {
                uint64_t size =
                    stat(full.c_str(), &info) == 0 ? (uint64_t)info.st_size : 0;
                files.push_back({name, size});
            }
        }
        closedir(handle);
    }
    std::sort(dirs.begin(), dirs.end(),
              [](const std::string& a, const std::string& b) {
                  return strcasecmp(a.c_str(), b.c_str()) < 0;
              });
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) {
                  return strcasecmp(a.first.c_str(), b.first.c_str()) < 0;
              });

    size_t visited = 0;
    for (const std::string& name : dirs) {
        if (visited++ >= (size_t)kMaxEntries) break;
        std::string full = joinPath(path, name);
        EntryRow* row = new EntryRow(
            name, std::string(), true,
            [this, full] { this->load(full); });
        this->listBox->addView(row);
        if (!firstRow) firstRow = row;
    }
    for (const auto& entry : files) {
        if (visited++ >= (size_t)kMaxEntries) break;
        std::string full = joinPath(path, entry.first);
        EntryRow* row = new EntryRow(
            entry.first, formatSize(entry.second), false,
            [this, full, name = entry.first] {
                auto callback = this->onPick;
                if (callback) callback(full, name);
                brls::Application::popActivity();
            });
        this->listBox->addView(row);
        if (!firstRow) firstRow = row;
    }

    if (!firstRow) {
        brls::Label* empty = new brls::Label();
        empty->setText("cloud/upload/dir_empty"_i18n);
        empty->setFontSize(16);
        empty->setTextColor(theme["cloud/text_secondary"]);
        empty->setMarginTop(12);
        empty->setMarginLeft(4);
        empty->setFocusable(true);
        empty->setHideHighlight(true);
        this->listBox->addView(empty);
        firstRow = empty;
    }
    brls::Application::giveFocus(firstRow);
}

}  // namespace cloud
