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
    /// Chapter body. Inline images appear as their own lines starting with
    /// `kBookImagePrefix`, followed by the image's path inside the archive.
    std::string text;
};

struct BookContent {
    bool ok = false;
    std::string error;
    std::string title;
    std::vector<BookChapter> chapters;
};

/// Prefix of the marker lines that stand for an inline image inside
/// BookChapter::text.
extern const char* const kBookImagePrefix;

/// Prefix of a heading line inside BookChapter::text: `"\x01H<level>\x01"`.
extern const char* const kBookHeadingPrefix;

/// Splits a heading marker line into its level (1-6) and text. Returns false
/// for ordinary lines.
bool parseBookHeading(const std::string& line, int* level,
                      std::string* text);

/// Loads a `.epub` or plain text (`.txt` / `.md`) file into chapters. Styles,
/// scripts and navigation documents are dropped; inline images become marker
/// lines the reader turns into picture pages. Returns `BookContent` with
/// `ok == false` and a human readable error on failure.
BookContent loadBookFile(const std::string& path);

/// Reads one image of an EPUB into `bytes`. Used by the reader for picture
/// pages: only the picture on screen is decoded, so a picture heavy book never
/// has to keep every illustration in memory.
bool loadBookImage(const std::string& bookPath, const std::string& entry,
                   std::string* bytes);

}  // namespace cloud
