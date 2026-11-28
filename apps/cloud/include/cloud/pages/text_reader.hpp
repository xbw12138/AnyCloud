//
// cloud: paged plain text reader for downloaded EPUB / TXT books.
//

#pragma once

#include <borealis.hpp>

#include <memory>
#include <string>
#include <vector>

#include "cloud/util/book_text.hpp"

namespace cloud {

class ReaderTextView;

class TextReaderPage : public brls::Box {
public:
    explicit TextReaderPage(std::string path);
    ~TextReaderPage() override;

    void onLayout() override;

private:
    /// One screen of the book: either wrapped text or a single illustration.
    struct ReaderPage {
        std::string text;
        /// Path of the picture inside the EPUB, empty for text pages.
        std::string image;
    };

    void applyBook(BookContent content);
    void rebuildPages();
    void showPage();
    /// Decodes the illustration of an image page and hands it to imageView.
    void showImage(const std::string& entry);
    void nextPage();
    void previousPage();
    void nextChapter();
    void previousChapter();
    void updateStatus();
    /// Y: chapter list, so a long book can be navigated by its table of
    /// contents instead of paging through everything.
    void showToc();

    std::string path;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    BookContent book;
    size_t chapterIndex = 0;
    int pageIndex = 0;
    std::vector<ReaderPage> pages;

    /// Cached layout metrics; pagination is redone only when they change.
    float lastWidth = 0;
    float lastHeight = 0;
    bool layoutReady = false;
    /// Set while pagination runs: sizing the picture view re-runs the whole
    /// Yoga layout synchronously, which would call onLayout() again and
    /// recurse forever while the page list is still being rebuilt.
    bool building = false;

    brls::Box* contentBox = nullptr;
    brls::Label* titleLabel = nullptr;
    brls::Label* statusLabel = nullptr;
    ReaderTextView* pageView = nullptr;
    brls::Image* imageView = nullptr;
    brls::Label* hintLabel = nullptr;
};

}  // namespace cloud
