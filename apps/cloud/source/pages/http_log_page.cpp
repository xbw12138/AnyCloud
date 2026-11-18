//
// cloud: in-app network log viewer.
//

#include "cloud/pages/http_log_page.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "nxkit/http_log.hpp"

using namespace brls::literals;

namespace cloud {

namespace {

const float kCardHeight = 120;
const float kCardGap    = 10;
const float kPadX       = 16;

std::string clockOf(int64_t seconds) {
    time_t value = (time_t)seconds;
    struct tm tmv {};
    localtime_r(&value, &tmv);
    char buf[32];
    strftime(buf, sizeof(buf), "%m-%d %H:%M:%S", &tmv);
    return buf;
}

std::string sizeOf(size_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double value = (double)bytes;
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        unit++;
    }
    char buf[48];
    if (unit == 0)
        snprintf(buf, sizeof(buf), "%d B", (int)bytes);
    else
        snprintf(buf, sizeof(buf), "%.1f %s", value, units[unit]);
    return buf;
}

std::string durationOf(int ms) {
    char buf[32];
    if (ms >= 1000)
        snprintf(buf, sizeof(buf), "%.1fs", (double)ms / 1000.0);
    else
        snprintf(buf, sizeof(buf), "%dms", ms);
    return buf;
}

/// Short status column: cache decisions carry no HTTP code.
std::string statusOf(const nxkit::HttpLogEntry& entry) {
    if (entry.method == "CACHE") return entry.ok ? "HIT" : "MISS";
    if (entry.status > 0) return std::to_string(entry.status);
    if (entry.code != 0) return "curl " + std::to_string(entry.code);
    return "ERR";
}

NVGcolor resultColor(const nxkit::HttpLogEntry& entry) {
    if (entry.method == "CACHE") return nvgRGB(86, 156, 255);
    if (entry.ok) return nvgRGB(66, 196, 126);
    return nvgRGB(238, 88, 86);
}

/// Draw `text` wrapped to `width`, at most `maxRows` lines.
void drawWrapped(NVGcontext* vg, const std::string& text, float x, float y,
                 float width, int maxRows, float lineHeight) {
    if (text.empty() || width <= 0) return;
    NVGtextRow rows[4];
    int count =
        nvgTextBreakLines(vg, text.c_str(), nullptr, width, rows, maxRows);
    for (int i = 0; i < count && i < maxRows; i++)
        nvgText(vg, x, y + i * lineHeight, rows[i].start, rows[i].end);
}

}  // namespace

/// Scrolling log list drawn with NanoVG: a few hundred entries would be too
/// heavy as individual borealis labels.
class HttpLogPage::LogView : public brls::View {
public:
    LogView() {
        this->setFocusable(true);
        this->setHideHighlight(true);

        // D-pad moves the selection, the list follows to keep it visible.
        this->registerAction(
            "", brls::BUTTON_NAV_DOWN,
            [this](brls::View*) {
                this->moveSelection(1);
                return true;
            },
            true, true);
        this->registerAction(
            "", brls::BUTTON_NAV_UP,
            [this](brls::View*) {
                this->moveSelection(-1);
                return true;
            },
            true, true);
    }

    void setEntries(std::vector<nxkit::HttpLogEntry> values) {
        this->entries = std::move(values);
        this->scroll = 0;
        this->selected = 0;
        this->invalidate();
    }

    const nxkit::HttpLogEntry* selectedEntry() const {
        if (this->selected >= this->entries.size()) return nullptr;
        return &this->entries[this->selected];
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (width <= 0 || height <= 0) return;

        brls::Theme theme = brls::Application::getTheme();
        nvgFontFaceId(vg, brls::Application::getDefaultFont());

        this->viewportHeight = height;
        if (this->entries.empty()) {
            nvgFontSize(vg, 18);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg, this->a(theme["cloud/text_secondary"]));
            nvgText(vg, x + width * 0.5f, y + height * 0.5f,
                    "cloud/log/empty"_i18n.c_str(), nullptr);
            return;
        }

        this->contentHeight =
            this->entries.size() * (kCardHeight + kCardGap) - kCardGap;
        this->scroll = std::min(this->scroll, this->maxScroll());
        if (this->scroll < 0) this->scroll = 0;

        nvgSave(vg);
        nvgIntersectScissor(vg, x, y, width, height);

        float cardY = y - this->scroll;
        for (size_t i = 0; i < this->entries.size(); i++) {
            if (cardY + kCardHeight >= y && cardY <= y + height)
                this->drawCard(vg, theme, this->entries[i], x, cardY, width,
                               i == this->selected);
            cardY += kCardHeight + kCardGap;
        }

        nvgRestore(vg);
    }

private:
    float maxScroll() const {
        return std::max(0.0f, this->contentHeight - this->viewportHeight);
    }

    void moveSelection(int delta) {
        if (this->entries.empty()) return;
        int next = (int)this->selected + delta;
        next = std::min((int)this->entries.size() - 1, std::max(0, next));
        if ((size_t)next == this->selected) return;
        this->selected = (size_t)next;

        // Keep the selected card inside the viewport.
        float top = this->selected * (kCardHeight + kCardGap);
        if (top < this->scroll) this->scroll = top;
        if (top + kCardHeight > this->scroll + this->viewportHeight)
            this->scroll = top + kCardHeight - this->viewportHeight;
        this->scroll = std::min(std::max(0.0f, this->scroll), this->maxScroll());
        this->invalidate();
    }

    void drawCard(NVGcontext* vg, brls::Theme& theme,
                  const nxkit::HttpLogEntry& entry, float x, float y,
                  float width, bool selected) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, kCardHeight, 14);
        nvgFillColor(vg, this->a(selected ? theme["cloud/surface_alt"]
                                          : theme["cloud/surface"]));
        nvgFill(vg);
        nvgStrokeColor(vg, this->a(selected ? theme["brls/accent"]
                                            : theme["cloud/border"]));
        nvgStrokeWidth(vg, selected ? 2.0f : 1.0f);
        nvgStroke(vg);

        const float innerWidth = width - kPadX * 2;
        const float baseline   = y + 30;

        // Header: time, method + status, then duration / size on the right.
        nvgFontSize(vg, 14);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
        nvgFillColor(vg, this->a(theme["cloud/text_secondary"]));
        std::string clock = clockOf(entry.time);
        nvgText(vg, x + kPadX, baseline, clock.c_str(), nullptr);

        std::string label = entry.method + "  " + statusOf(entry);
        nvgFillColor(vg, this->a(resultColor(entry)));
        nvgText(vg, x + kPadX + 108, baseline, label.c_str(), nullptr);

        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE);
        nvgFillColor(vg, this->a(theme["cloud/text_secondary"]));
        std::string metrics =
            durationOf(entry.durationMs) + "   " + sizeOf(entry.bytes);
        nvgText(vg, x + width - kPadX, baseline, metrics.c_str(), nullptr);

        // Request url, at most two lines.
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
        nvgFontSize(vg, 13);
        nvgFillColor(vg, this->a(theme["cloud/text"]));
        float urlTop = baseline + 21;
        drawWrapped(vg, entry.url, x + kPadX, urlTop, innerWidth, 2, 17);

        // Content type / error / response snippet.
        std::string detail = entry.note;
        if (!entry.error.empty()) {
            if (!detail.empty()) detail += "  ·  ";
            detail += entry.error;
        }
        if (!entry.snippet.empty()) {
            if (!detail.empty()) detail += "  ·  ";
            // Only a short prefix is needed for the two-line preview.
            detail += entry.snippet.substr(0, 400);
        }
        // Cards show a flowing two-line preview; the detail page keeps the
        // original line breaks.
        std::replace(detail.begin(), detail.end(), '\n', ' ');
        std::replace(detail.begin(), detail.end(), '\r', ' ');
        nvgFillColor(vg, this->a(theme["cloud/text_secondary"]));
        drawWrapped(vg, detail, x + kPadX, urlTop + 17 * 2 + 2, innerWidth, 2,
                    17);
    }

    std::vector<nxkit::HttpLogEntry> entries;
    size_t selected      = 0;
    float scroll         = 0;
    float contentHeight  = 0;
    float viewportHeight = 0;
};

brls::View* HttpLogPage::createContentView() {
    brls::Theme theme = brls::Application::getTheme();

    brls::Box* root = new brls::Box(brls::Axis::COLUMN);
    root->setGrow(1);
    root->setPadding(26, 40, 20, 40);
    root->setBackgroundColor(theme["cloud/bg"]);

    brls::Box* header = new brls::Box(brls::Axis::ROW);
    header->setAlignItems(brls::AlignItems::CENTER);

    brls::Label* title = new brls::Label();
    title->setText("cloud/log/title"_i18n);
    title->setFontSize(28);
    title->setTextColor(theme["cloud/text"]);
    title->setHeight(36);
    header->addView(title);

    brls::Label* hint = new brls::Label();
    hint->setText("cloud/log/hint"_i18n);
    hint->setFontSize(14);
    hint->setTextColor(theme["cloud/text_secondary"]);
    hint->setHeight(20);
    hint->setMarginLeft(20);
    hint->setGrow(1);
    header->addView(hint);

    this->summary = new brls::Label();
    this->summary->setFontSize(14);
    this->summary->setTextColor(theme["cloud/text_secondary"]);
    this->summary->setHeight(20);
    header->addView(this->summary);

    root->addView(header);

    this->list = new LogView();
    this->list->setGrow(1);
    this->list->setMarginTop(12);
    root->addView(this->list);

    this->list->registerAction("cloud/log/open"_i18n, brls::BUTTON_A,
                               [this](brls::View*) {
                                   if (const auto* entry =
                                           this->list->selectedEntry())
                                       this->showDetail(*entry);
                                   return true;
                               });
    this->list->registerAction("cloud/log/refresh"_i18n, brls::BUTTON_X,
                               [this](brls::View*) {
                                   this->rebuild();
                                   return true;
                               });
    this->list->registerAction("cloud/log/clear"_i18n, brls::BUTTON_Y,
                               [this](brls::View*) {
                                   nxkit::HttpLog::instance().clear();
                                   this->rebuild();
                                   brls::Application::notify(
                                       "cloud/log/cleared"_i18n);
                                   return true;
                               });
    this->list->registerAction(
        "hints/back"_i18n, brls::BUTTON_B,
        [](brls::View*) {
            brls::Application::popActivity();
            return true;
        },
        true);

    this->rebuild();
    return root;
}

namespace {

struct DetailRow {
    std::string text;
    float fontSize = 14;
    int kind = 3;  // 0 title, 1 partial, 2 section, 3 body
    float baseline = 0;
};

/// Full-screen entry detail. Renders the response body in full with a custom
/// NanoVG layout: wrapping a multi-hundred-KB JSON in a borealis Label would
/// re-shape the whole string every frame.
class LogDetailView : public brls::View {
public:
    explicit LogDetailView(nxkit::HttpLogEntry value)
        : entry(std::move(value)) {
        this->setFocusable(true);
        this->setHideHighlight(true);
        this->registerAction(
            "", brls::BUTTON_NAV_DOWN,
            [this](brls::View*) {
                this->scrollBy(70);
                return true;
            },
            true, true);
        this->registerAction(
            "", brls::BUTTON_NAV_UP,
            [this](brls::View*) {
                this->scrollBy(-70);
                return true;
            },
            true, true);
        this->registerAction(
            "hints/back"_i18n, brls::BUTTON_B,
            [](brls::View*) {
                brls::Application::popActivity();
                return true;
            },
            true);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (width <= 0 || height <= 0) return;

        brls::Theme theme = brls::Application::getTheme();
        nvgFontFaceId(vg, brls::Application::getDefaultFont());
        this->layout(vg, width);
        this->viewportHeight = height;
        this->scroll = std::min(std::max(0.0f, this->scroll), this->maxScroll());

        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillColor(vg, this->a(theme["cloud/bg"]));
        nvgFill(vg);

        nvgSave(vg);
        nvgIntersectScissor(vg, x, y, width, height);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
        for (const auto& row : this->rows) {
            float baseline = y - this->scroll + row.baseline;
            if (baseline < y - row.fontSize) continue;
            if (baseline > y + height + row.fontSize) break;

            NVGcolor color = theme["cloud/text"];
            if (row.kind == 0)
                color = resultColor(this->entry);
            else if (row.kind == 1 || row.kind == 2)
                color = theme["cloud/text_secondary"];
            nvgFontSize(vg, row.fontSize);
            nvgFillColor(vg, this->a(color));
            nvgText(vg, x + kPad, baseline, row.text.c_str(), nullptr);
        }
        nvgRestore(vg);
    }

private:
    static constexpr float kPad = 24;

    float maxScroll() const {
        return std::max(0.0f, this->contentHeight - this->viewportHeight);
    }

    void scrollBy(float delta) {
        this->scroll =
            std::min(std::max(0.0f, this->scroll + delta), this->maxScroll());
        this->invalidate();
    }

    void pushWrapped(NVGcontext* vg, const std::string& text, float fontSize,
                     int kind, float width) {
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t nl = text.find('\n', pos);
            std::string line =
                text.substr(pos, nl == std::string::npos ? std::string::npos
                                                          : nl - pos);
            while (!line.empty() && line.back() == '\r') line.pop_back();

            if (!line.empty()) {
                nvgFontSize(vg, fontSize);
                const char* start = line.c_str();
                const char* end = start + line.size();
                while (start < end && this->rows.size() < 20000) {
                    NVGtextRow row;
                    int count =
                        nvgTextBreakLines(vg, start, end, width, &row, 1);
                    if (count <= 0) break;
                    DetailRow item;
                    item.text.assign(row.start, row.end);
                    item.fontSize = fontSize;
                    item.kind = kind;
                    item.baseline = this->cursor + fontSize;
                    this->rows.push_back(std::move(item));
                    this->cursor += fontSize * 1.45f;
                    if (row.next <= start) break;
                    start = row.next;
                }
            } else {
                this->cursor += fontSize * 0.9f;
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        this->cursor += 6;
    }

    void layout(NVGcontext* vg, float width) {
        if (this->layoutWidth == width) return;
        this->layoutWidth = width;
        this->rows.clear();
        this->cursor = 24;

        const float innerWidth = std::max(1.0f, width - kPad * 2);
        this->pushWrapped(vg, this->entry.method + "  " + statusOf(this->entry),
                          22, 0, innerWidth);
        this->pushWrapped(vg, clockOf(this->entry.time) + "  ·  " +
                                  durationOf(this->entry.durationMs) + "  ·  " +
                                  sizeOf(this->entry.bytes),
                          14, 1, innerWidth);

        this->cursor += 8;
        this->pushWrapped(vg, "cloud/log/detail_url"_i18n, 13, 2, innerWidth);
        this->pushWrapped(vg, this->entry.url, 15, 3, innerWidth);

        std::string note = this->entry.note;
        if (!this->entry.error.empty()) {
            if (!note.empty()) note += "\n";
            note += this->entry.error;
        }
        if (!note.empty()) {
            this->cursor += 8;
            this->pushWrapped(vg, "cloud/log/detail_note"_i18n, 13, 2,
                              innerWidth);
            this->pushWrapped(vg, note, 15, 3, innerWidth);
        }

        if (!this->entry.snippet.empty()) {
            this->cursor += 8;
            this->pushWrapped(vg, "cloud/log/detail_body"_i18n, 13, 2,
                              innerWidth);
            this->pushWrapped(vg, this->entry.snippet, 14, 3, innerWidth);
        }

        this->contentHeight = this->cursor + 24;
    }

    nxkit::HttpLogEntry entry;
    std::vector<DetailRow> rows;
    float cursor         = 0;
    float scroll         = 0;
    float contentHeight  = 0;
    float viewportHeight = 0;
    float layoutWidth    = -1;
};

}  // namespace

void HttpLogPage::showDetail(const nxkit::HttpLogEntry& entry) {
    brls::Application::pushActivity(
        new brls::Activity(new LogDetailView(entry)));
}

void HttpLogPage::rebuild() {
    std::vector<nxkit::HttpLogEntry> entries =
        nxkit::HttpLog::instance().snapshot();

    size_t failed = 0;
    size_t cached = 0;
    for (const auto& entry : entries) {
        if (!entry.ok) failed++;
        if (entry.method == "CACHE") cached++;
    }

    if (this->summary)
        this->summary->setText(brls::getStr("cloud/log/summary", entries.size(),
                                            failed, cached));

    // Newest first: the interesting request is almost always the last one.
    std::reverse(entries.begin(), entries.end());
    if (this->list) this->list->setEntries(std::move(entries));
}

}  // namespace cloud
