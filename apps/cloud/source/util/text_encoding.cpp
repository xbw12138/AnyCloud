//
// cloud: text encoding helpers for downloaded subtitles.
//

#include "cloud/util/text_encoding.hpp"

#include <cstdint>

namespace cloud {

extern const uint16_t kGbkTable[126 * 191];

namespace {

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

bool isValidUtf8(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = (unsigned char)text[i];
        size_t extra = 0;
        if (c < 0x80) {
            i++;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
            if (c < 0xC2) return false;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            if (c > 0xF4) return false;
        } else {
            return false;
        }
        if (i + extra >= text.size()) return false;
        for (size_t k = 1; k <= extra; k++) {
            if (((unsigned char)text[i + k] & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

std::string utf16ToUtf8(const std::string& bytes, bool bigEndian) {
    std::string out;
    out.reserve(bytes.size());
    size_t i = 2;  // skip the BOM
    uint32_t pending = 0;
    for (; i + 1 < bytes.size(); i += 2) {
        unsigned char a = (unsigned char)bytes[i];
        unsigned char b = (unsigned char)bytes[i + 1];
        uint32_t unit = bigEndian ? ((a << 8) | b) : ((b << 8) | a);

        if (unit >= 0xD800 && unit <= 0xDBFF) {
            pending = unit;
            continue;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF && pending) {
            uint32_t cp = 0x10000 + ((pending - 0xD800) << 10) +
                          (unit - 0xDC00);
            pending = 0;
            appendUtf8(out, cp);
            continue;
        }
        if (pending) {
            appendUtf8(out, 0xFFFD);
            pending = 0;
        }
        appendUtf8(out, unit);
    }
    return out;
}

std::string gbkToUtf8(const std::string& bytes) {
    std::string out;
    out.reserve(bytes.size() + bytes.size() / 2);
    for (size_t i = 0; i < bytes.size(); i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (c < 0x80) {
            out += (char)c;
            continue;
        }
        if (c >= 0x81 && c <= 0xFE && i + 1 < bytes.size()) {
            unsigned char trail = (unsigned char)bytes[i + 1];
            if (trail >= 0x40 && trail <= 0xFE && trail != 0x7F) {
                uint16_t cp = kGbkTable[(c - 0x81) * 191 + (trail - 0x40)];
                i++;
                if (cp) {
                    appendUtf8(out, cp);
                    continue;
                }
            }
        }
        out += (char)0xEF;  // replacement character U+FFFD
        out += (char)0xBF;
        out += (char)0xBD;
    }
    return out;
}

}  // namespace

std::string subtitleToUtf8(const std::string& bytes) {
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF &&
        (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
        return bytes.substr(3);

    if (bytes.size() >= 2) {
        unsigned char a = (unsigned char)bytes[0];
        unsigned char b = (unsigned char)bytes[1];
        if (a == 0xFF && b == 0xFE) return utf16ToUtf8(bytes, false);
        if (a == 0xFE && b == 0xFF) return utf16ToUtf8(bytes, true);
    }

    if (isValidUtf8(bytes)) return bytes;
    return gbkToUtf8(bytes);
}

}  // namespace cloud
