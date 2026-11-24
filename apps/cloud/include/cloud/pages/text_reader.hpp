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

class TextReaderPage : public brls::Box {
public:
    explicit TextReaderPage(std::string path);
    ~TextReaderPage() override;

    void onLayout() override;

private:
    void applyBook(BookContent content);
    void rebuildPages();
    void showPage();
    void nextPage();
    void previousPage();
    void nextChapter();
    void previousChapter();
    void updateStatus();

    std::string path;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    BookContent book;
    size_t chapterIndex = 0;
    int pageIndex = 0;
    std::vector<std::string> pages;

    /// Cached layout metrics; pagination is redone only when they change.
    float lastWidth = 0;
    float lastHeight = 0;
    bool layoutReady = false;

    brls::Box* contentBox = nullptr;
    brls::Label* titleLabel = nullptr;
    brls::Label* statusLabel = nullptr;
    brls::Label* pageLabel = nullptr;
    brls::Label* hintLabel = nullptr;
};

}  // namespace cloud
