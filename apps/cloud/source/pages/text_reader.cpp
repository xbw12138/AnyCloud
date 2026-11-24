//
// cloud: paged plain text reader for downloaded EPUB / TXT books.
//

#include "cloud/pages/text_reader.hpp"

#include <cmath>
#include <unordered_map>

#include <borealis/core/thread.hpp>

#include "cpr/async.h"

using namespace brls::literals;

namespace cloud {

namespace {

constexpr float kFontSize = 20.0f;
constexpr float kLineHeight = 1.65f;

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
            // A blank line separates paragraphs and makes long books easier
            // to scan.
            if (!lines.empty() && !lines.back().empty()) lines.push_back("");
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

    this->pageLabel = new brls::Label();
    this->pageLabel->setFontSize(kFontSize);
    this->pageLabel->setLineHeight(kLineHeight);
    // Without this the label draws a single truncated line, so every "page"
    // looked like one row even though the pagination was correct.
    this->pageLabel->setIsWrapping(true);
    this->pageLabel->setWidthPercentage(100);
    this->pageLabel->setTextColor(theme["cloud/text"]);
    this->contentBox->addView(this->pageLabel);

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
    if (!this->book.ok) {
        this->titleLabel->setText("cloud/reader/error"_i18n);
        this->statusLabel->setText(this->book.error);
        this->pageLabel->setText("");
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

    if (!this->book.ok || !this->contentBox) return;
    float width = this->contentBox->getWidth();
    float height = this->contentBox->getHeight();
    if (width <= 0 || height <= 0) return;

    this->layoutReady = true;
    if (std::fabs(width - this->lastWidth) < 0.5f &&
        std::fabs(height - this->lastHeight) < 0.5f && !this->pages.empty())
        return;

    this->lastWidth = width;
    this->lastHeight = height;
    this->rebuildPages();
}

void TextReaderPage::rebuildPages() {
    this->pages.clear();
    if (!this->book.ok || this->book.chapters.empty()) {
        this->showPage();
        return;
    }
    if (this->chapterIndex >= this->book.chapters.size())
        this->chapterIndex = this->book.chapters.size() - 1;

    const BookChapter& chapter = this->book.chapters[this->chapterIndex];
    int font = this->pageLabel->getFont();
    float width = this->contentBox->getWidth();
    float height = this->contentBox->getHeight();
    if (width <= 0) width = this->getWidth();
    if (height <= 0) height = 400;

    // A small safety margin absorbs per-glyph width rounding so nanovg never
    // re-wraps a line we already broke.
    std::vector<std::string> lines =
        wrapText(chapter.text, width * 0.99f, font, kFontSize);
    int linesPerPage = (int)(height / (kFontSize * kLineHeight));
    if (linesPerPage < 3) linesPerPage = 3;

    for (size_t start = 0; start < lines.size();
         start += (size_t)linesPerPage) {
        std::string page;
        size_t end = std::min(lines.size(), start + (size_t)linesPerPage);
        for (size_t i = start; i < end; i++) {
            if (i > start) page.push_back('\n');
            page += lines[i];
        }
        // Drop trailing blank lines so pages do not look half empty.
        while (!page.empty() && page.back() == '\n') page.pop_back();
        if (page.empty() && !this->pages.empty()) continue;
        this->pages.push_back(std::move(page));
    }
    if (this->pages.empty()) this->pages.push_back(std::string());

    if (this->pageIndex >= (int)this->pages.size())
        this->pageIndex = (int)this->pages.size() - 1;
    if (this->pageIndex < 0) this->pageIndex = 0;
    this->showPage();
}

void TextReaderPage::showPage() {
    if (!this->book.ok) return;
    if (this->pages.empty()) {
        this->pageLabel->setText("cloud/reader/empty"_i18n);
        this->updateStatus();
        return;
    }
    if (this->pageIndex < 0) this->pageIndex = 0;
    if (this->pageIndex >= (int)this->pages.size())
        this->pageIndex = (int)this->pages.size() - 1;
    this->pageLabel->setText(this->pages[this->pageIndex]);
    this->updateStatus();
}

void TextReaderPage::updateStatus() {
    std::string page = brls::getStr("cloud/reader/page", this->pageIndex + 1,
                                    (int)this->pages.size());
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

}  // namespace cloud
