//
// cloud: plain text extraction for downloaded EPUB / TXT books.
//

#include "cloud/util/book_text.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cloud/util/text_encoding.hpp"

namespace cloud {

namespace {

constexpr uint32_t kEocdSig = 0x06054b50;
constexpr uint32_t kZip64LocatorSig = 0x07064b50;
constexpr uint32_t kZip64EocdSig = 0x06064b50;
constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint32_t kLocalSig = 0x04034b50;

uint16_t rd16(const unsigned char* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t rd32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

uint64_t rd64(const unsigned char* p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

bool readAt(FILE* file, uint64_t offset, void* out, size_t size) {
    if (fseeko(file, (off_t)offset, SEEK_SET) != 0) return false;
    return fread(out, 1, size, file) == size;
}

bool readWholeFile(const std::string& path, std::string* out) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return false;
    bool ok = true;
    if (fseeko(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    off_t end = ftello(file);
    if (end < 0) {
        fclose(file);
        return false;
    }
    out->resize((size_t)end);
    if (end > 0 && !readAt(file, 0, &(*out)[0], (size_t)end)) ok = false;
    fclose(file);
    return ok;
}

std::string lowerExtension(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string base =
        slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot == std::string::npos) return std::string();
    std::string ext = base.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

std::string trim(const std::string& text) {
    size_t start = 0;
    size_t end = text.size();
    auto isSpace = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (start < end && isSpace((unsigned char)text[start])) start++;
    while (end > start && isSpace((unsigned char)text[end - 1])) end--;
    return text.substr(start, end - start);
}

std::string lowerCopy(const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });
    return out;
}

size_t findInsensitive(const std::string& haystack, const std::string& needle,
                       size_t from) {
    if (needle.empty() || from >= haystack.size()) return std::string::npos;
    std::string lowerHay = lowerCopy(haystack);
    std::string lowerNeedle = lowerCopy(needle);
    return lowerHay.find(lowerNeedle, from);
}

std::string decodeEntities(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text[i] != '&') {
            out.push_back(text[i++]);
            continue;
        }
        size_t semi = text.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 10) {
            out.push_back(text[i++]);
            continue;
        }
        std::string body = text.substr(i + 1, semi - i - 1);
        bool handled = true;
        if (body == "amp") {
            out.push_back('&');
        } else if (body == "lt") {
            out.push_back('<');
        } else if (body == "gt") {
            out.push_back('>');
        } else if (body == "quot") {
            out.push_back('"');
        } else if (body == "apos") {
            out.push_back('\'');
        } else if (body == "nbsp") {
            out.push_back(' ');
        } else if (body == "mdash") {
            out += "\xE2\x80\x94";
        } else if (body == "ndash") {
            out += "\xE2\x80\x93";
        } else if (body == "hellip") {
            out += "\xE2\x80\xA6";
        } else if (body.size() > 1 && body[0] == '#') {
            uint32_t value = 0;
            bool valid = true;
            if (body[1] == 'x' || body[1] == 'X') {
                for (size_t k = 2; k < body.size(); k++) {
                    char c = body[k];
                    if (!isxdigit((unsigned char)c)) {
                        valid = false;
                        break;
                    }
                    value = value * 16 +
                            (uint32_t)(isdigit((unsigned char)c)
                                           ? c - '0'
                                           : (tolower((unsigned char)c) - 'a' + 10));
                }
            } else {
                for (size_t k = 1; k < body.size(); k++) {
                    char c = body[k];
                    if (!isdigit((unsigned char)c)) {
                        valid = false;
                        break;
                    }
                    value = value * 10 + (uint32_t)(c - '0');
                }
            }
            if (!valid || value == 0 || value > 0x10FFFF) {
                handled = false;
            } else {
                if (value < 0x80) {
                    out.push_back((char)value);
                } else if (value < 0x800) {
                    out.push_back((char)(0xC0 | (value >> 6)));
                    out.push_back((char)(0x80 | (value & 0x3F)));
                } else if (value < 0x10000) {
                    out.push_back((char)(0xE0 | (value >> 12)));
                    out.push_back((char)(0x80 | ((value >> 6) & 0x3F)));
                    out.push_back((char)(0x80 | (value & 0x3F)));
                } else {
                    out.push_back((char)(0xF0 | (value >> 18)));
                    out.push_back((char)(0x80 | ((value >> 12) & 0x3F)));
                    out.push_back((char)(0x80 | ((value >> 6) & 0x3F)));
                    out.push_back((char)(0x80 | (value & 0x3F)));
                }
            }
        } else {
            handled = false;
        }
        if (handled) {
            i = semi + 1;
        } else {
            out.push_back(text[i++]);
        }
    }
    return out;
}

std::string stripTags(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool inTag = false;
    for (char c : text) {
        if (c == '<') {
            inTag = true;
        } else if (c == '>') {
            inTag = false;
        } else if (!inTag) {
            out.push_back(c);
        }
    }
    return trim(decodeEntities(out));
}

/// Turns an XHTML document into reflowable plain text.
std::string htmlToText(const std::string& html) {
    size_t start = 0;
    size_t body = findInsensitive(html, "<body", 0);
    if (body != std::string::npos) {
        size_t end = html.find('>', body);
        if (end != std::string::npos) start = end + 1;
    } else {
        // No body element: drop the head so the <title> is not duplicated.
        size_t head = findInsensitive(html, "</head", 0);
        if (head != std::string::npos) {
            size_t end = html.find('>', head);
            if (end != std::string::npos) start = end + 1;
        }
    }

    static const char* kBlockTags[] = {
        "p",   "div", "br",  "hr", "h1", "h2",   "h3",   "h4",
        "h5",  "h6",  "li",  "tr", "ul", "ol",   "table", "section",
        "article", "blockquote", "pre", "dd", "dt"};

    auto isBlock = [&](const std::string& name) {
        std::string lower = lowerCopy(name);
        for (const char* tag : kBlockTags)
            if (lower == tag) return true;
        return false;
    };

    std::string raw;
    raw.reserve(html.size());
    size_t i = start;
    while (i < html.size()) {
        if (html[i] != '<') {
            size_t next = html.find('<', i);
            if (next == std::string::npos) next = html.size();
            std::string chunk = html.substr(i, next - i);
            // Source formatting (newlines between tags) must not create
            // paragraph breaks; only block level tags do.
            for (char& c : chunk)
                if (c == '\r' || c == '\n' || c == '\t') c = ' ';
            raw += decodeEntities(chunk);
            i = next;
            continue;
        }
        if (html.compare(i, 4, "<!--") == 0) {
            size_t end = html.find("-->", i + 4);
            i = end == std::string::npos ? html.size() : end + 3;
            continue;
        }
        size_t close = html.find('>', i + 1);
        if (close == std::string::npos) break;
        std::string tag = html.substr(i + 1, close - i - 1);
        i = close + 1;

        size_t nameStart = 0;
        bool closing = false;
        while (nameStart < tag.size() &&
               isspace((unsigned char)tag[nameStart]))
            nameStart++;
        if (nameStart < tag.size() && tag[nameStart] == '/') {
            closing = true;
            nameStart++;
        }
        size_t nameEnd = nameStart;
        while (nameEnd < tag.size() &&
               (isalnum((unsigned char)tag[nameEnd]) ||
                tag[nameEnd] == ':' || tag[nameEnd] == '-' ||
                tag[nameEnd] == '_'))
            nameEnd++;
        std::string name = tag.substr(nameStart, nameEnd - nameStart);
        std::string lower = lowerCopy(name);

        if (lower == "script" || lower == "style") {
            size_t end = findInsensitive(html, "</" + lower, i);
            if (end == std::string::npos) {
                i = html.size();
            } else {
                size_t gt = html.find('>', end);
                i = gt == std::string::npos ? html.size() : gt + 1;
            }
            continue;
        }
        if (isBlock(lower)) raw.push_back('\n');
    }

    // Normalize whitespace line by line so the reader can re-wrap freely.
    std::string out;
    out.reserve(raw.size());
    std::string line;
    int blankLines = 0;
    auto flushLine = [&] {
        std::string cleaned;
        cleaned.reserve(line.size());
        bool lastSpace = false;
        for (unsigned char c : line) {
            if (c == ' ' || c == '\t') {
                if (!lastSpace && !cleaned.empty()) cleaned.push_back(' ');
                lastSpace = true;
            } else {
                cleaned.push_back((char)c);
                lastSpace = false;
            }
        }
        cleaned = trim(cleaned);
        if (cleaned.empty()) {
            blankLines++;
            if (blankLines == 1 && !out.empty()) out.push_back('\n');
            return;
        }
        blankLines = 0;
        out += cleaned;
        out.push_back('\n');
    };
    for (char c : raw) {
        if (c == '\r') continue;
        if (c == '\n') {
            flushLine();
            line.clear();
        } else {
            line.push_back(c);
        }
    }
    flushLine();
    return trim(out);
}

std::string extractTitle(const std::string& html) {
    size_t start = findInsensitive(html, "<title", 0);
    if (start != std::string::npos) {
        size_t open = html.find('>', start);
        size_t end = open == std::string::npos
                         ? std::string::npos
                         : findInsensitive(html, "</title", open);
        if (open != std::string::npos && end != std::string::npos) {
            std::string title =
                stripTags(html.substr(open + 1, end - open - 1));
            if (!title.empty()) return title;
        }
    }
    for (const char* tag : {"h1", "h2", "h3"}) {
        size_t begin = findInsensitive(html, std::string("<") + tag, 0);
        if (begin == std::string::npos) continue;
        size_t open = html.find('>', begin);
        size_t end = open == std::string::npos
                         ? std::string::npos
                         : findInsensitive(html, std::string("</") + tag,
                                           open);
        if (open == std::string::npos || end == std::string::npos) continue;
        std::string title = stripTags(html.substr(open + 1, end - open - 1));
        if (!title.empty()) return title;
    }
    return std::string();
}

/// Reads a member of a zip archive without unpacking anything to disk.
class ZipArchive {
public:
    ~ZipArchive() {
        if (this->file) fclose(this->file);
    }

    bool open(const std::string& path, std::string* error) {
        this->file = fopen(path.c_str(), "rb");
        if (!this->file) {
            *error = "cannot open file";
            return false;
        }
        if (fseeko(this->file, 0, SEEK_END) != 0) {
            *error = "cannot read file";
            return false;
        }
        off_t end = ftello(this->file);
        if (end < 22) {
            *error = "not a zip archive";
            return false;
        }
        this->size = (uint64_t)end;

        size_t tailSize = (size_t)std::min<uint64_t>(this->size, 65557);
        std::string tail(tailSize, '\0');
        if (!readAt(this->file, this->size - tailSize, &tail[0], tailSize)) {
            *error = "cannot read archive";
            return false;
        }
        size_t eocd = std::string::npos;
        for (size_t i = tailSize - 22 + 1; i-- > 0;) {
            if (rd32((const unsigned char*)tail.data() + i) == kEocdSig) {
                eocd = i;
                break;
            }
        }
        if (eocd == std::string::npos) {
            *error = "not a zip archive";
            return false;
        }

        uint64_t totalEntries =
            rd16((const unsigned char*)tail.data() + eocd + 10);
        uint64_t centralSize =
            rd32((const unsigned char*)tail.data() + eocd + 12);
        uint64_t centralOffset =
            rd32((const unsigned char*)tail.data() + eocd + 16);
        if (totalEntries == 0xFFFF || centralSize == 0xFFFFFFFF ||
            centralOffset == 0xFFFFFFFF) {
            uint64_t eocdPosition = this->size - tailSize + eocd;
            unsigned char locator[20];
            if (eocdPosition >= 20 &&
                readAt(this->file, eocdPosition - 20, locator,
                       sizeof(locator)) &&
                rd32(locator) == kZip64LocatorSig) {
                unsigned char record[56];
                uint64_t zip64Offset = rd64(locator + 8);
                if (readAt(this->file, zip64Offset, record, sizeof(record)) &&
                    rd32(record) == kZip64EocdSig) {
                    totalEntries = rd64(record + 32);
                    centralSize = rd64(record + 40);
                    centralOffset = rd64(record + 48);
                }
            }
        }
        if (centralOffset > this->size ||
            centralSize > this->size - centralOffset ||
            centralSize > 64ULL * 1024ULL * 1024ULL) {
            *error = "corrupt central directory";
            return false;
        }

        std::string central((size_t)centralSize, '\0');
        if (!readAt(this->file, centralOffset, &central[0], central.size())) {
            *error = "cannot read central directory";
            return false;
        }

        size_t pos = 0;
        while (pos + 46 <= central.size()) {
            const unsigned char* header =
                (const unsigned char*)central.data() + pos;
            if (rd32(header) != kCentralSig) break;
            Entry entry;
            entry.flags = rd16(header + 8);
            entry.method = rd16(header + 10);
            entry.compressedSize = rd32(header + 20);
            entry.size = rd32(header + 24);
            uint16_t nameLen = rd16(header + 28);
            uint16_t extraLen = rd16(header + 30);
            uint16_t commentLen = rd16(header + 32);
            entry.localOffset = rd32(header + 42);
            if (pos + 46 + nameLen + extraLen + commentLen > central.size())
                break;
            entry.name = central.substr(pos + 46, nameLen);
            std::replace(entry.name.begin(), entry.name.end(), '\\', '/');
            this->entries.push_back(std::move(entry));
            pos += 46 + (size_t)nameLen + extraLen + commentLen;
        }
        if (this->entries.empty()) {
            *error = "archive has no entries";
            return false;
        }
        return true;
    }

    bool read(const std::string& name, std::string* out,
              std::string* error) const {
        const Entry* entry = nullptr;
        for (const Entry& candidate : this->entries)
            if (candidate.name == name) {
                entry = &candidate;
                break;
            }
        if (!entry) {
            *error = "missing " + name;
            return false;
        }
        if (entry->flags & 0x0001) {
            *error = "encrypted archive";
            return false;
        }
        if (entry->size > 32ULL * 1024ULL * 1024ULL ||
            entry->compressedSize > 32ULL * 1024ULL * 1024ULL) {
            *error = "entry too large";
            return false;
        }

        unsigned char local[30];
        if (!readAt(this->file, entry->localOffset, local, sizeof(local)) ||
            rd32(local) != kLocalSig) {
            *error = "corrupt local header";
            return false;
        }
        uint16_t nameLen = rd16(local + 26);
        uint16_t extraLen = rd16(local + 28);
        uint64_t dataOffset = entry->localOffset + 30 + nameLen + extraLen;
        if (dataOffset > this->size ||
            entry->compressedSize > this->size - dataOffset) {
            *error = "invalid entry size";
            return false;
        }

        std::string compressed((size_t)entry->compressedSize, '\0');
        if (!compressed.empty() &&
            !readAt(this->file, dataOffset, &compressed[0],
                    compressed.size())) {
            *error = "cannot read entry";
            return false;
        }
        if (entry->method == 0) {
            if (compressed.size() != entry->size) {
                *error = "invalid stored entry";
                return false;
            }
            *out = std::move(compressed);
            return true;
        }
        if (entry->method != 8) {
            *error = "unsupported compression";
            return false;
        }

        z_stream stream{};
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
            *error = "cannot initialise zlib";
            return false;
        }
        out->assign((size_t)entry->size, '\0');
        stream.next_in = (Bytef*)compressed.data();
        stream.avail_in = (uInt)compressed.size();
        stream.next_out = (Bytef*)&(*out)[0];
        stream.avail_out = (uInt)out->size();
        int ret = inflate(&stream, Z_FINISH);
        size_t written = out->size() - stream.avail_out;
        inflateEnd(&stream);
        if (ret != Z_STREAM_END || written != entry->size) {
            *error = "corrupt deflate data";
            out->clear();
            return false;
        }
        return true;
    }

private:
    struct Entry {
        std::string name;
        uint16_t flags = 0;
        uint16_t method = 0;
        uint64_t compressedSize = 0;
        uint64_t size = 0;
        uint64_t localOffset = 0;
    };

    FILE* file = nullptr;
    uint64_t size = 0;
    std::vector<Entry> entries;
};

std::string attribute(const std::string& tag, const std::string& name) {
    std::string lower = lowerCopy(tag);
    std::string needle = lowerCopy(name);
    size_t pos = 0;
    while ((pos = lower.find(needle, pos)) != std::string::npos) {
        bool boundary =
            pos == 0 || isspace((unsigned char)lower[pos - 1]) ||
            lower[pos - 1] == '<';
        size_t after = pos + needle.size();
        while (after < lower.size() && isspace((unsigned char)lower[after]))
            after++;
        if (boundary && after < lower.size() && lower[after] == '=') {
            after++;
            while (after < lower.size() &&
                   isspace((unsigned char)lower[after]))
                after++;
            if (after < lower.size() &&
                (lower[after] == '"' || lower[after] == '\'')) {
                char quote = lower[after];
                size_t end = tag.find(quote, after + 1);
                if (end == std::string::npos) return std::string();
                return tag.substr(after + 1, end - after - 1);
            }
        }
        pos += needle.size();
    }
    return std::string();
}

std::string percentDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '%' && i + 2 < text.size() &&
            isxdigit((unsigned char)text[i + 1]) &&
            isxdigit((unsigned char)text[i + 2])) {
            auto hex = [](char c) {
                if (c >= '0' && c <= '9') return c - '0';
                return tolower((unsigned char)c) - 'a' + 10;
            };
            out.push_back((char)(hex(text[i + 1]) * 16 + hex(text[i + 2])));
            i += 2;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::string directoryOf(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

/// Resolves `href` against the directory of the OPF file, collapsing "." and
/// ".." segments.
std::string resolvePath(const std::string& baseDir, const std::string& href) {
    std::string decoded = percentDecode(href);
    size_t fragment = decoded.find_first_of("#?");
    if (fragment != std::string::npos) decoded.resize(fragment);
    std::string combined =
        (baseDir.empty() ? std::string() : baseDir + "/") + decoded;
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= combined.size()) {
        size_t end = combined.find('/', start);
        std::string part = combined.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    std::string out;
    for (const std::string& part : parts) {
        if (!out.empty()) out += '/';
        out += part;
    }
    return out;
}

struct ManifestItem {
    std::string href;
    std::string mediaType;
};

BookContent loadEpub(const std::string& path) {
    BookContent content;

    ZipArchive archive;
    if (!archive.open(path, &content.error)) return content;

    std::string container;
    if (!archive.read("META-INF/container.xml", &container, &content.error))
        return content;

    size_t rootfile = findInsensitive(container, "<rootfile", 0);
    // Skip the enclosing <rootfiles> element.
    while (rootfile != std::string::npos) {
        size_t after = rootfile + 9;
        if (after < container.size() &&
            (isspace((unsigned char)container[after]) ||
             container[after] == '>' || container[after] == '/'))
            break;
        rootfile = findInsensitive(container, "<rootfile", after);
    }
    if (rootfile == std::string::npos) {
        content.error = "missing rootfile";
        return content;
    }
    size_t rootEnd = container.find('>', rootfile);
    std::string opfPath =
        attribute(container.substr(rootfile, rootEnd - rootfile + 1),
                  "full-path");
    if (opfPath.empty()) {
        content.error = "missing package path";
        return content;
    }
    opfPath = percentDecode(opfPath);
    std::string opfDir = directoryOf(opfPath);

    std::string opf;
    if (!archive.read(opfPath, &opf, &content.error)) return content;

    // Book title.
    size_t titleStart = findInsensitive(opf, "<dc:title", 0);
    if (titleStart != std::string::npos) {
        size_t open = opf.find('>', titleStart);
        size_t end = open == std::string::npos
                         ? std::string::npos
                         : findInsensitive(opf, "</dc:title", open);
        if (open != std::string::npos && end != std::string::npos)
            content.title = stripTags(opf.substr(open + 1, end - open - 1));
    }

    // Manifest: id -> href / media type.
    std::vector<std::pair<std::string, ManifestItem>> manifest;
    size_t manifestPos = findInsensitive(opf, "<manifest", 0);
    size_t manifestEnd = manifestPos == std::string::npos
                             ? std::string::npos
                             : findInsensitive(opf, "</manifest", manifestPos);
    if (manifestPos != std::string::npos && manifestEnd != std::string::npos) {
        size_t pos = manifestPos;
        while (true) {
            size_t item = findInsensitive(opf, "<item", pos);
            if (item == std::string::npos || item >= manifestEnd) break;
            size_t itemEnd = opf.find('>', item);
            if (itemEnd == std::string::npos || itemEnd > manifestEnd) break;
            std::string tag = opf.substr(item, itemEnd - item + 1);
            std::string id = attribute(tag, "id");
            std::string href = attribute(tag, "href");
            std::string mediaType = attribute(tag, "media-type");
            if (!id.empty() && !href.empty())
                manifest.push_back(
                    {id, ManifestItem{percentDecode(href), mediaType}});
            pos = itemEnd + 1;
        }
    }

    // Spine: reading order.
    std::vector<std::string> hrefs;
    size_t spinePos = findInsensitive(opf, "<spine", 0);
    size_t spineEnd = spinePos == std::string::npos
                          ? std::string::npos
                          : findInsensitive(opf, "</spine", spinePos);
    if (spinePos != std::string::npos && spineEnd != std::string::npos) {
        size_t pos = spinePos;
        while (true) {
            size_t item = findInsensitive(opf, "<itemref", pos);
            if (item == std::string::npos || item >= spineEnd) break;
            size_t itemEnd = opf.find('>', item);
            if (itemEnd == std::string::npos || itemEnd > spineEnd) break;
            std::string tag = opf.substr(item, itemEnd - item + 1);
            std::string idref = attribute(tag, "idref");
            for (const auto& entry : manifest) {
                if (entry.first != idref) continue;
                const std::string& mediaType = entry.second.mediaType;
                bool textual = mediaType.empty() ||
                               mediaType == "application/xhtml+xml" ||
                               mediaType == "text/html" ||
                               mediaType == "text/xml";
                if (textual) hrefs.push_back(entry.second.href);
                break;
            }
            pos = itemEnd + 1;
        }
    }
    if (hrefs.empty()) {
        // Broken spine: fall back to every XHTML entry in the manifest.
        for (const auto& entry : manifest) {
            const std::string& mediaType = entry.second.mediaType;
            if (mediaType == "application/xhtml+xml" ||
                mediaType == "text/html")
                hrefs.push_back(entry.second.href);
        }
    }
    if (hrefs.empty()) {
        content.error = "no readable content";
        return content;
    }

    for (const std::string& href : hrefs) {
        std::string entryPath = resolvePath(opfDir, href);
        std::string error;
        std::string bytes;
        if (!archive.read(entryPath, &bytes, &error)) continue;
        std::string html = subtitleToUtf8(bytes);
        std::string text = htmlToText(html);
        if (text.empty()) continue;
        BookChapter chapter;
        chapter.title = extractTitle(html);
        chapter.text = std::move(text);
        content.chapters.push_back(std::move(chapter));
    }
    if (content.chapters.empty()) {
        content.error = "no readable content";
        return content;
    }
    if (content.title.empty()) {
        std::string base = path;
        size_t slash = base.find_last_of("/\\");
        if (slash != std::string::npos) base = base.substr(slash + 1);
        size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) base = base.substr(0, dot);
        content.title = base;
    }
    content.ok = true;
    return content;
}

/// Splits a long plain text into chapter sized chunks so that pagination stays
/// responsive and page numbers stay meaningful.
std::vector<BookChapter> splitText(const std::string& text) {
    const size_t kChunk = 8000;
    std::vector<BookChapter> chapters;
    if (text.size() <= kChunk * 2) {
        chapters.push_back(BookChapter{std::string(), text});
        return chapters;
    }
    size_t start = 0;
    while (start < text.size()) {
        size_t end = std::min(text.size(), start + kChunk);
        if (end < text.size()) {
            size_t newline = text.find_last_of('\n', end);
            if (newline != std::string::npos && newline > start)
                end = newline + 1;
            else
                while (end < text.size() &&
                       ((unsigned char)text[end] & 0xC0) == 0x80)
                    end++;
        }
        std::string chunk = text.substr(start, end - start);
        if (!trim(chunk).empty()) chapters.push_back({std::string(), chunk});
        start = end;
    }
    return chapters;
}

BookContent loadText(const std::string& path) {
    BookContent content;
    std::string bytes;
    if (!readWholeFile(path, &bytes)) {
        content.error = "cannot open file";
        return content;
    }
    std::string text = subtitleToUtf8(bytes);
    // Keep paragraph structure but drop trailing whitespace on every line.
    std::string normalized;
    normalized.reserve(text.size());
    std::string line;
    auto flush = [&] {
        normalized += trim(line);
        normalized.push_back('\n');
        line.clear();
    };
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n')
            flush();
        else
            line.push_back(c);
    }
    if (!line.empty()) flush();

    std::string trimmed = trim(normalized);
    if (trimmed.empty()) {
        content.error = "no readable content";
        return content;
    }
    content.chapters = splitText(trimmed);
    content.ok = !content.chapters.empty();
    if (!content.ok) content.error = "no readable content";

    std::string base = path;
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    content.title = base;
    return content;
}

}  // namespace

BookContent loadBookFile(const std::string& path) {
    if (lowerExtension(path) == "epub") return loadEpub(path);
    return loadText(path);
}

}  // namespace cloud
