//
// cloud: paged plain text reader for downloaded EPUB / TXT books.
//

#include "cloud/pages/text_reader.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include <borealis/core/thread.hpp>

#include "cpr/async.h"

using namespace brls::literals;

namespace cloud {

namespace {

/// On-device diagnostics, same file the player and the SMB layer log to.
void readerLog(const std::string& line) {
#ifdef __SWITCH__
    FILE* file = fopen("sdmc:/switch/nxkit/qr_debug.log", "a");
#else
    FILE* file = fopen("/tmp/qr_debug.log", "a");
#endif
    if (!file) return;
    fprintf(file, "%s\n", line.c_str());
    fclose(file);
}

constexpr float kFontSize = 20.0f;
/// Baseline distance of the page body. NanoVG's own line height (font metrics
/// times lineHeight) came out far larger, so the reader positions lines itself.
constexpr float kLinePixels = 30.0f;
/// Extra space for a paragraph break: much smaller than a full blank line, a
/// whole empty line between paragraphs looked loose and uneven.
constexpr float kParagraphGap = 12.0f;

/// Headings are drawn larger than the body and get their own line advance.
float headingSize(int level) { return level <= 2 ? 28.0f : 24.0f; }
float headingAdvance(int level) { return level <= 2 ? 44.0f : 38.0f; }

size_t nextCodepoint(const std::string& text, size_t index, uint32_t* cp) {
    unsigned char c = (unsigned char)text[index];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    int extra = 0;
    uint32_t value = 0;
    if ((c & 0xE0) == 0xC0) {
        extra = 1;
        value = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2;
        value = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3;
        value = c & 0x07;
    } else {
        *cp = '?';
        return 1;
    }
    if (index + (size_t)extra >= text.size()) {
        *cp = '?';
        return text.size() - index;
    }
    for (int i = 1; i <= extra; i++) {
        unsigned char next = (unsigned char)text[index + i];
        if ((next & 0xC0) != 0x80) {
            *cp = '?';
            return 1;
        }
        value = (value << 6) | (next & 0x3F);
    }
    *cp = value;
    return (size_t)extra + 1;
}

std::string encodeCodepoint(uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
    return out;
}

/// Measures glyph advances once per code point. Splitting by code point also
/// gives CJK text break opportunities that nanovg's space based wrapping
/// would never find.
struct GlyphWidths {
    NVGcontext* vg = nullptr;
    int font = 0;
    float size = 0;
    std::unordered_map<uint32_t, float> cache;

    float get(uint32_t cp) {
        auto it = cache.find(cp);
        if (it != cache.end()) return it->second;
        std::string glyph = encodeCodepoint(cp);
        float bounds[4] = {0, 0, 0, 0};
        nvgTextBounds(vg, 0, 0, glyph.c_str(), nullptr, bounds);
        float width = bounds[2] - bounds[0];
        if (width < 0) width = 0;
        cache.emplace(cp, width);
        return width;
    }
};

std::vector<std::string> wrapText(const std::string& text, float maxWidth,
                                  int font, float fontSize) {
    std::vector<std::string> lines;
    NVGcontext* vg = brls::Application::getNVGContext();
    nvgFontFaceId(vg, font);
    nvgFontSize(vg, fontSize);

    GlyphWidths glyphs;
    glyphs.vg = vg;
    glyphs.font = font;
    glyphs.size = fontSize;

    std::string line;
    float lineWidth = 0;

    auto flush = [&] {
        // Trim trailing spaces, keep intentional indentation out of the way.
        while (!line.empty() && line.back() == ' ') line.pop_back();
        // The extracted text already marks paragraph breaks with an empty
        // line; collapse runs so a paragraph never opens a double gap.
        if (!(line.empty() && !lines.empty() && lines.back().empty()))
            lines.push_back(line);
        line.clear();
        lineWidth = 0;
    };

    for (size_t i = 0; i < text.size();) {
        uint32_t cp = 0;
        size_t length = nextCodepoint(text, i, &cp);
        std::string glyph = text.substr(i, length);
        i += length;

        if (cp == '\r') continue;
        if (cp == '\n') {
            flush();
            continue;
        }

        float width = glyphs.get(cp);
        if (!line.empty() && lineWidth + width > maxWidth) {
            size_t lastSpace = line.find_last_of(' ');
            if (lastSpace != std::string::npos) {
                std::string rest = line.substr(lastSpace + 1);
                line.resize(lastSpace);
                flush();
                line = rest;
                for (size_t k = 0; k < rest.size();) {
                    uint32_t restCp = 0;
                    size_t restLen = nextCodepoint(rest, k, &restCp);
                    lineWidth += glyphs.get(restCp);
                    k += restLen;
                }
            } else {
                flush();
            }
        }
        line += glyph;
        lineWidth += width;
    }
    if (!line.empty() || lines.empty()) flush();
    return lines;
}

}  // namespace

/// Page body painted directly with NanoVG. A box label measures its line
/// height from the font metrics (about 44 px here instead of the 30 px the
/// pagination expects) and silently falls back to one horizontally scrolling
/// line when the text does not fit, so the reader draws the lines it already
/// wrapped itself.
class ReaderTextView : public brls::View {
public:
    ReaderTextView() { this->setFocusable(false); }

    void setPage(std::string text) {
        this->text = std::move(text);
        this->invalidate();
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (width <= 0 || height <= 0 || this->text.empty()) return;

        nvgSave(vg);
        nvgScissor(vg, x, y, width, height);
        nvgFontFaceId(vg, brls::Application::getDefaultFont());
        nvgFontSize(vg, kFontSize);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg, brls::Application::getTheme()["cloud/text"]);

        float ty = y;
        size_t pos = 0;
        while (pos < this->text.size()) {
            size_t newline = this->text.find('\n', pos);
            std::string line = this->text.substr(
                pos, newline == std::string::npos ? std::string::npos
                                                  : newline - pos);

            int level = 0;
            std::string heading;
            const bool isHeading = parseBookHeading(line, &level, &heading);
            float advance = kLinePixels;
            if (isHeading) {
                advance = headingAdvance(level);
            } else if (line.empty()) {
                // Paragraph breaks cost less than a full line.
                advance = kParagraphGap;
            }
            if (ty + advance > y + height + 0.5f) break;

            if (isHeading && !heading.empty()) {
                const float size = headingSize(level);
                nvgFontSize(vg, size);
                // The bundled fonts have no bold face: a hair of offset fakes
                // the heavier weight headings need.
                nvgText(vg, x, ty, heading.c_str(), nullptr);
                nvgText(vg, x + 0.7f, ty, heading.c_str(), nullptr);
                nvgFontSize(vg, kFontSize);
            } else if (!line.empty()) {
                nvgText(vg, x, ty, line.c_str(), nullptr);
            }
            ty += advance;
            if (newline == std::string::npos) break;
            pos = newline + 1;
        }
        nvgRestore(vg);
    }

private:
    std::string text;
};

TextReaderPage::TextReaderPage(std::string path) : path(std::move(path)) {
    this->setAxis(brls::Axis::COLUMN);
    this->setGrow(1);
    this->setFocusable(true);
    this->setHideHighlight(true);
    this->setBackgroundColor(brls::Application::getTheme()["cloud/bg"]);
    this->setPadding(24, 48, 18, 48);

    brls::Theme theme = brls::Application::getTheme();

    this->titleLabel = new brls::Label();
    this->titleLabel->setFontSize(16);
    this->titleLabel->setHeight(22);
    this->titleLabel->setTextColor(theme["cloud/text_secondary"]);
    this->addView(this->titleLabel);

    this->statusLabel = new brls::Label();
    this->statusLabel->setFontSize(12);
    this->statusLabel->setHeight(18);
    this->statusLabel->setMarginTop(2);
    this->statusLabel->setTextColor(theme["cloud/text_secondary"]);
    this->addView(this->statusLabel);

    this->contentBox = new brls::Box(brls::Axis::COLUMN);
    this->contentBox->setWidthPercentage(100);
    this->contentBox->setGrow(1);
    this->contentBox->setMarginTop(14);
    this->contentBox->setClipsToBounds(true);
    this->addView(this->contentBox);

    this->pageView = new ReaderTextView();
    this->pageView->setWidthPercentage(100);
    this->contentBox->addView(this->pageView);

    // Illustration pages reuse the same area: one picture is decoded at a
    // time and its texture is released when the next page is shown.
    this->imageView = new brls::Image();
    this->imageView->setScalingType(brls::ImageScalingType::FIT);
    this->imageView->setImageAlign(brls::ImageAlignment::CENTER);
    this->imageView->setVisibility(brls::Visibility::GONE);
    this->contentBox->addView(this->imageView);

    this->hintLabel = new brls::Label();
    this->hintLabel->setFontSize(12);
    this->hintLabel->setHeight(18);
    this->hintLabel->setMarginTop(8);
    this->hintLabel->setTextColor(theme["cloud/text_secondary"]);
    this->hintLabel->setText("cloud/reader/hint"_i18n);
    this->addView(this->hintLabel);

    this->titleLabel->setText("cloud/reader/loading"_i18n);
    this->statusLabel->setText("");

    this->registerAction(
        "cloud/reader/prev"_i18n, brls::BUTTON_LB,
        [this](brls::View*) -> bool {
            this->previousPage();
            return true;
        });
    this->registerAction(
        "cloud/reader/next"_i18n, brls::BUTTON_RB,
        [this](brls::View*) -> bool {
            this->nextPage();
            return true;
        });
    this->registerAction(
        "", brls::BUTTON_NAV_LEFT,
        [this](brls::View*) -> bool {
            this->previousPage();
            return true;
        },
        true, true);
    this->registerAction(
        "", brls::BUTTON_NAV_RIGHT,
        [this](brls::View*) -> bool {
            this->nextPage();
            return true;
        },
        true, true);
    this->registerAction(
        "", brls::BUTTON_NAV_UP,
        [this](brls::View*) -> bool {
            this->previousPage();
            return true;
        },
        true, true);
    this->registerAction(
        "", brls::BUTTON_NAV_DOWN,
        [this](brls::View*) -> bool {
            this->nextPage();
            return true;
        },
        true, true);
    this->registerAction(
        "cloud/reader/prev_chapter"_i18n, brls::BUTTON_LT,
        [this](brls::View*) -> bool {
            this->previousChapter();
            return true;
        });
    this->registerAction(
        "cloud/reader/next_chapter"_i18n, brls::BUTTON_RT,
        [this](brls::View*) -> bool {
            this->nextChapter();
            return true;
        });
    this->registerAction(
        "cloud/reader/toc"_i18n, brls::BUTTON_Y,
        [this](brls::View*) -> bool {
            this->showToc();
            return true;
        });
    this->registerAction(
        "", brls::BUTTON_B,
        [](brls::View*) -> bool {
            brls::Application::popActivity();
            return true;
        },
        true);

    // Parsing and text extraction run on cpr's worker pool; bare detached
    // std::threads are not safe on the Switch.
    cpr::async([this, path = this->path, alive = this->alive] {
        BookContent content = loadBookFile(path);
        brls::sync([this, alive, content = std::move(content)]() mutable {
            if (!*alive) return;
            this->applyBook(std::move(content));
        });
    });
}

TextReaderPage::~TextReaderPage() { *this->alive = false; }

void TextReaderPage::applyBook(BookContent content) {
    this->book = std::move(content);
    readerLog("reader: open '" + this->path + "' ok=" +
              (this->book.ok ? "1" : "0") + " err=" + this->book.error +
              " chapters=" + std::to_string(this->book.chapters.size()));
    if (!this->book.ok) {
        this->titleLabel->setText("cloud/reader/error"_i18n);
        this->statusLabel->setText(this->book.error);
        this->pageView->setVisibility(brls::Visibility::VISIBLE);
        this->pageView->setPage("");
        if (this->imageView)
            this->imageView->setVisibility(brls::Visibility::GONE);
        return;
    }
    this->titleLabel->setText(this->book.title);
    this->chapterIndex = 0;
    this->pageIndex = 0;
    this->pages.clear();
    if (this->layoutReady)
        this->rebuildPages();
    else
        this->statusLabel->setText("cloud/reader/loading"_i18n);
}

void TextReaderPage::onLayout() {
    brls::Box::onLayout();

    if (this->building) return;
    if (!this->book.ok || !this->contentBox) return;

    // Percentage widths are resolved unreliably inside a flex grown box: the
    // page label measured the whole text as one endless line, so every page
    // showed a single row. Give the content column and the label the exact
    // pixel width instead (screen width minus the page padding).
    const float inset = this->contentBox->getX() - this->getX();
    float width = this->getWidth() - inset * 2.0f;
    if (width <= 0) width = this->contentBox->getWidth();
    if (width <= 0) return;

    this->building = true;
    if (std::fabs(this->contentBox->getWidth() - width) > 0.5f)
        this->contentBox->setWidth(width);
    if (std::fabs(this->pageView->getWidth() - width) > 0.5f)
        this->pageView->setWidth(width);

    float height = this->contentBox->getHeight();
    if (height <= 0) {
        this->building = false;
        return;
    }
    if (std::fabs(this->pageView->getHeight() - height) > 0.5f)
        this->pageView->setHeight(height);

    this->layoutReady = true;
    if (std::fabs(width - this->lastWidth) < 0.5f &&
        std::fabs(height - this->lastHeight) < 0.5f && !this->pages.empty()) {
        this->building = false;
        return;
    }

    this->lastWidth = width;
    this->lastHeight = height;
    this->rebuildPages();
    this->building = false;
}

void TextReaderPage::rebuildPages() {
    this->pages.clear();
    if (!this->book.ok || this->book.chapters.empty()) {
        this->showPage();
        return;
    }
    if (this->chapterIndex >= this->book.chapters.size())
        this->chapterIndex = this->book.chapters.size() - 1;

    // Sizing the picture view runs the root layout synchronously; block the
    // resulting onLayout() until pagination is complete.
    this->building = true;

    const BookChapter& chapter = this->book.chapters[this->chapterIndex];
    float width = this->contentBox->getWidth();
    float height = this->contentBox->getHeight();
    if (width <= 0) width = this->getWidth();
    if (height <= 0) height = 400;

    if (this->pageView) {
        this->pageView->setWidth(width);
        this->pageView->setHeight(height);
    }
    if (this->imageView) {
        this->imageView->setWidth(width);
        this->imageView->setHeight(height);
    }

    // A small safety margin absorbs per-glyph width rounding so nanovg never
    // re-wraps a line we already broke. Pages are filled by pixel height so a
    // paragraph gap can be shorter than a full line.
    const float pageHeight = height - 4.0f;

    std::string buffer;                  // raw text of the current paragraph run
    std::vector<std::string> items;      // wrapped display lines of the run
    std::vector<std::string> pageLines;  // lines of the page being filled
    float used = 0;

    auto emitPage = [&] {
        if (pageLines.empty()) return;
        std::string page;
        for (size_t i = 0; i < pageLines.size(); i++) {
            if (i > 0) page.push_back('\n');
            page += pageLines[i];
        }
        while (!page.empty() && page.back() == '\n') page.pop_back();
        this->pages.push_back(ReaderPage{std::move(page), std::string()});
        pageLines.clear();
        used = 0;
    };

    auto wrapInto = [&](const std::string& source, int level) {
        if (source.empty()) return;
        const bool heading = level > 0;
        const float size = heading ? headingSize(level) : kFontSize;
        std::vector<std::string> lines = wrapText(
            source, width * 0.99f, brls::Application::getDefaultFont(), size);
        const std::string marker =
            heading ? std::string(kBookHeadingPrefix) +
                          std::string(1, (char)('0' + level)) + '\x01'
                    : std::string();
        for (std::string& wrapped : lines)
            items.push_back(marker + wrapped);
    };

    auto flushBuffer = [&] {
        if (buffer.empty()) return;
        std::string source = std::move(buffer);
        buffer.clear();
        wrapInto(source, 0);
    };

    auto paginate = [&] {
        for (const std::string& item : items) {
            int level = 0;
            std::string headingText;
            const bool heading = parseBookHeading(item, &level, &headingText);
            float advance = kLinePixels;
            if (heading)
                advance = headingAdvance(level);
            else if (item.empty())
                advance = kParagraphGap;
            // Never open a page with the empty line of a paragraph break.
            if (pageLines.empty() && item.empty()) continue;
            if (!pageLines.empty() && used + advance > pageHeight) emitPage();
            pageLines.push_back(item);
            used += advance;
        }
        items.clear();
    };

    // Picture markers become pages of their own; headings keep their level so
    // the page body can draw them larger.
    const std::string& text = chapter.text;
    const size_t markerLen = std::strlen(kBookImagePrefix);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t newline = text.find('\n', pos);
        std::string line = text.substr(
            pos, newline == std::string::npos ? std::string::npos
                                              : newline - pos);
        if (line.compare(0, markerLen, kBookImagePrefix) == 0) {
            flushBuffer();
            paginate();
            emitPage();
            this->pages.push_back(
                ReaderPage{std::string(), line.substr(markerLen)});
        } else {
            int level = 0;
            std::string heading;
            if (parseBookHeading(line, &level, &heading)) {
                flushBuffer();
                wrapInto(heading, level);
            } else {
                buffer += line;
                buffer.push_back('\n');
            }
        }
        if (newline == std::string::npos) break;
        pos = newline + 1;
    }
    flushBuffer();
    paginate();
    emitPage();

    if (this->pages.empty())
        this->pages.push_back(ReaderPage{std::string(), std::string()});

    if (this->pageIndex >= (int)this->pages.size())
        this->pageIndex = (int)this->pages.size() - 1;
    if (this->pageIndex < 0) this->pageIndex = 0;
    readerLog("reader: layout w=" + std::to_string((int)width) + " h=" +
              std::to_string((int)height) + " chapter='" + chapter.title +
              "' pages=" + std::to_string(this->pages.size()));
    this->showPage();
    this->building = false;
}

void TextReaderPage::showImage(const std::string& entry) {
    if (!this->imageView) return;
    std::string bytes;
    if (!loadBookImage(this->path, entry, &bytes)) {
        this->imageView->clear();
        readerLog("reader: image missing '" + entry + "'");
        return;
    }
    // Free the previous texture first so flipping through a picture book keeps
    // one decoded image in memory at a time.
    this->imageView->clear();
    this->imageView->setFreeTexture(true);
    this->imageView->setImageFromMem((unsigned char*)bytes.data(),
                                     (int)bytes.size());
}

void TextReaderPage::showPage() {
    if (!this->book.ok) return;
    if (this->pages.empty()) {
        this->pageView->setVisibility(brls::Visibility::VISIBLE);
        if (this->imageView)
            this->imageView->setVisibility(brls::Visibility::GONE);
        this->pageView->setPage("cloud/reader/empty"_i18n);
        this->updateStatus();
        return;
    }
    if (this->pageIndex < 0) this->pageIndex = 0;
    if (this->pageIndex >= (int)this->pages.size())
        this->pageIndex = (int)this->pages.size() - 1;

    const ReaderPage& page = this->pages[this->pageIndex];
    if (page.image.empty()) {
        if (this->imageView)
            this->imageView->setVisibility(brls::Visibility::GONE);
        this->pageView->setVisibility(brls::Visibility::VISIBLE);
        this->pageView->setPage(page.text);
    } else {
        this->pageView->setVisibility(brls::Visibility::GONE);
        if (this->imageView)
            this->imageView->setVisibility(brls::Visibility::VISIBLE);
        this->showImage(page.image);
    }
    this->updateStatus();
}

void TextReaderPage::updateStatus() {
    std::string page = brls::getStr("cloud/reader/page", this->pageIndex + 1,
                                    (int)this->pages.size());
    if (this->pageIndex >= 0 && this->pageIndex < (int)this->pages.size() &&
        !this->pages[this->pageIndex].image.empty())
        page = "cloud/reader/picture"_i18n + "   " + page;
    std::string chapter;
    if (this->chapterIndex < this->book.chapters.size())
        chapter = this->book.chapters[this->chapterIndex].title;
    if (chapter.empty() && this->book.chapters.size() > 1)
        chapter = std::to_string(this->chapterIndex + 1) + " / " +
                  std::to_string(this->book.chapters.size());
    this->statusLabel->setText(
        chapter.empty() ? page : chapter + "   " + page);
}

void TextReaderPage::nextPage() {
    if (!this->book.ok || this->pages.empty()) return;
    if (this->pageIndex + 1 < (int)this->pages.size()) {
        this->pageIndex++;
        this->showPage();
        return;
    }
    this->nextChapter();
}

void TextReaderPage::previousPage() {
    if (!this->book.ok || this->pages.empty()) return;
    if (this->pageIndex > 0) {
        this->pageIndex--;
        this->showPage();
        return;
    }
    if (this->chapterIndex == 0) return;
    this->chapterIndex--;
    this->pageIndex = 0;
    this->rebuildPages();
    this->pageIndex = (int)this->pages.size() - 1;
    this->showPage();
}

void TextReaderPage::nextChapter() {
    if (!this->book.ok) return;
    if (this->chapterIndex + 1 >= this->book.chapters.size()) return;
    this->chapterIndex++;
    this->pageIndex = 0;
    this->rebuildPages();
}

void TextReaderPage::previousChapter() {
    if (!this->book.ok || this->chapterIndex == 0) return;
    this->chapterIndex--;
    this->pageIndex = 0;
    this->rebuildPages();
}

void TextReaderPage::showToc() {
    if (!this->book.ok || this->book.chapters.empty()) return;

    std::vector<std::string> names;
    names.reserve(this->book.chapters.size());
    for (size_t i = 0; i < this->book.chapters.size(); i++) {
        std::string title = this->book.chapters[i].title;
        if (title.empty())
            title = brls::getStr("cloud/reader/chapter_n", (int)i + 1);
        names.push_back((i == this->chapterIndex ? "▶ " : "   ") + title);
    }
    brls::Dropdown* dropdown = new brls::Dropdown(
        "cloud/reader/toc"_i18n, names,
        [this, alive = this->alive](int selected) {
            if (!*alive) return;
            if (selected < 0 ||
                selected >= (int)this->book.chapters.size())
                return;
            this->chapterIndex = (size_t)selected;
            this->pageIndex = 0;
            this->rebuildPages();
        },
        (int)this->chapterIndex);
    brls::Application::pushActivity(new brls::Activity(dropdown));
}

}  // namespace cloud
