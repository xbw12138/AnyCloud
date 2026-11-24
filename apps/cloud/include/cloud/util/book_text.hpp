//
// cloud: plain text extraction for downloaded EPUB / TXT books.
//

#pragma once

#include <string>
#include <vector>

namespace cloud {

struct BookChapter {
    /// Chapter heading, may be empty when the source has no usable title.
    std::string title;
    std::string text;
};

struct BookContent {
    bool ok = false;
    std::string error;
    std::string title;
    std::vector<BookChapter> chapters;
};

/// Loads a `.epub` or plain text (`.txt` / `.md`) file into chapters. Images,
/// styles, scripts and other non-text content are ignored. Returns
/// `BookContent` with `ok == false` and a human readable error on failure.
BookContent loadBookFile(const std::string& path);

}  // namespace cloud
