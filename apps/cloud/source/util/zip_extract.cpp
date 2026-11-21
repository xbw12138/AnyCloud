//
// cloud: minimal zip extraction (stored + deflate entries).
//

#include "cloud/util/zip_extract.hpp"

#include <zlib.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "cloud/util/text_encoding.hpp"

namespace cloud {

namespace {

constexpr uint32_t kEocdSig = 0x06054b50;
constexpr uint32_t kZip64LocatorSig = 0x07064b50;
constexpr uint32_t kZip64EocdSig = 0x06064b50;
constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint32_t kLocalSig = 0x04034b50;
constexpr size_t kChunk = 64 * 1024;

uint16_t read16(const unsigned char* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t read32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

uint64_t read64(const unsigned char* p) {
    return (uint64_t)read32(p) | ((uint64_t)read32(p + 4) << 32);
}

bool readAt(FILE* file, uint64_t offset, void* out, size_t size) {
    if (fseeko(file, (off_t)offset, SEEK_SET) != 0) return false;
    return fread(out, 1, size, file) == size;
}

bool fileSize(FILE* file, uint64_t* size) {
    if (fseeko(file, 0, SEEK_END) != 0) return false;
    off_t end = ftello(file);
    if (end < 0) return false;
    *size = (uint64_t)end;
    return true;
}

bool pathExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool isDirectory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool ensureDir(const std::string& path) {
    if (path.empty()) return false;
    if (isDirectory(path)) return true;
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        std::string parent = path.substr(0, slash);
        if (!isDirectory(parent)) ensureDir(parent);
    }
    if (mkdir(path.c_str(), 0777) == 0) return true;
    return isDirectory(path);
}

std::string parentDir(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string shortHash(const std::string& text) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 16777619u;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%06x", hash & 0xFFFFFF);
    return buf;
}

/// Keeps the bytes FAT/exFAT accept, collapses the rest into underscores.
/// Names made only of non-ASCII characters get a stable hash stem so two
/// different entries do not end up on the same path.
std::string sanitizeComponent(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (c == '/' || c == '\\' || c == ':' || c < 0x20 || c == '?' ||
            c == '*' || c == '"' || c == '<' || c == '>' || c == '|') {
            out.push_back('_');
        } else {
            out.push_back((char)c);
        }
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))
        out.pop_back();
    if (out.size() > 180) {
        out.resize(180);
        while (!out.empty() && ((unsigned char)out.back() & 0xC0) == 0x80)
            out.pop_back();
        if (!out.empty() && (unsigned char)out.back() >= 0xC0) out.pop_back();
    }
    if (out.empty()) out = "file";
    return out;
}

/// ASCII-only fallback used when FAT refuses the original UTF-8 name.
std::string asciiComponent(const std::string& raw) {
    std::string out;
    bool lastUnderscore = false;
    for (unsigned char c : raw) {
        bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                       c == '-';
        if (allowed) {
            out.push_back((char)c);
            lastUnderscore = false;
        } else if (!lastUnderscore) {
            out.push_back('_');
            lastUnderscore = true;
        }
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))
        out.pop_back();

    size_t dot = out.find_last_of('.');
    std::string stem = dot == std::string::npos ? out : out.substr(0, dot);
    std::string ext = dot == std::string::npos ? "" : out.substr(dot);
    if (stem.find_first_not_of("_-") == std::string::npos)
        out = "_" + shortHash(raw) + ext;

    if (out.size() > 80) {
        size_t mark = out.find_last_of('.');
        std::string extension =
            (mark != std::string::npos && out.size() - mark <= 10)
                ? out.substr(mark)
                : std::string();
        out = out.substr(0, 80 - extension.size()) + extension;
    }
    if (out.empty()) out = "file";
    return out;
}

/// Turns an archive name into a safe relative path. Returns an empty string
/// for unsafe entries (absolute paths, ".." segments).
std::string sanitizeRelative(const std::string& raw, bool* isDir,
                             bool* unsafe) {
    std::string name = raw;
    for (char& c : name)
        if (c == '\\') c = '/';
    *isDir = !name.empty() && name.back() == '/';
    *unsafe = false;

    std::string out;
    size_t start = 0;
    while (start <= name.size()) {
        size_t end = name.find('/', start);
        std::string part = name.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (end == std::string::npos)
            start = name.size() + 1;
        else
            start = end + 1;

        if (part.empty() || part == ".") continue;
        if (part == "..") {
            *unsafe = true;
            return std::string();
        }
        if (part.size() == 2 && part[1] == ':') continue;  // "C:" prefix
        std::string clean = sanitizeComponent(part);
        if (!out.empty()) out += '/';
        out += clean;
    }
    return out;
}

std::string asciiRelative(const std::string& relative) {
    std::string out;
    size_t start = 0;
    while (start <= relative.size()) {
        size_t end = relative.find('/', start);
        std::string part = relative.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty()) {
            if (!out.empty()) out += '/';
            out += asciiComponent(part);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

std::string uniquePath(const std::string& path) {
    if (!pathExists(path)) return path;
    size_t slash = path.find_last_of('/');
    size_t dot = path.find_last_of('.');
    std::string stem = path;
    std::string extension;
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        stem = path.substr(0, dot);
        extension = path.substr(dot);
    }
    for (int i = 2; i < 10000; i++) {
        std::string candidate =
            stem + " (" + std::to_string(i) + ")" + extension;
        if (!pathExists(candidate)) return candidate;
    }
    return path;
}

struct Entry {
    std::string name;
    uint16_t flags = 0;
    uint16_t method = 0;
    uint32_t crc = 0;
    uint64_t compressedSize = 0;
    uint64_t size = 0;
    uint64_t localOffset = 0;
};

bool parseZip64Extra(const std::string& data, size_t extraPos, size_t extraLen,
                     Entry* entry) {
    size_t pos = extraPos;
    size_t end = extraPos + extraLen;
    while (pos + 4 <= end) {
        uint16_t id = read16((const unsigned char*)data.data() + pos);
        uint16_t length = read16((const unsigned char*)data.data() + pos + 2);
        pos += 4;
        if (pos + length > end) break;
        if (id == 0x0001) {
            const unsigned char* p = (const unsigned char*)data.data() + pos;
            size_t remaining = length;
            if (entry->size == 0xFFFFFFFF && remaining >= 8) {
                entry->size = read64(p);
                p += 8;
                remaining -= 8;
            }
            if (entry->compressedSize == 0xFFFFFFFF && remaining >= 8) {
                entry->compressedSize = read64(p);
                p += 8;
                remaining -= 8;
            }
            if (entry->localOffset == 0xFFFFFFFF && remaining >= 8)
                entry->localOffset = read64(p);
            break;
        }
        pos += length;
    }
    return true;
}

bool writeBytes(FILE* out, const unsigned char* data, size_t size,
                uLong* crc) {
    if (size == 0) return true;
    if (fwrite(data, 1, size, out) != size) return false;
    *crc = crc32(*crc, data, (uInt)size);
    return true;
}

using ByteProgress = std::function<void(uint64_t delta)>;

bool copyStored(FILE* src, uint64_t offset, uint64_t size, FILE* out,
                uLong* crc, const std::function<bool()>& cancelled,
                const ByteProgress& onBytes, std::string* error) {
    if (fseeko(src, (off_t)offset, SEEK_SET) != 0) {
        *error = "cannot seek archive";
        return false;
    }
    std::vector<unsigned char> buffer(kChunk);
    uint64_t remaining = size;
    while (remaining > 0) {
        if (cancelled && cancelled()) {
            *error = "cancelled";
            return false;
        }
        size_t take = (size_t)std::min<uint64_t>(remaining, buffer.size());
        if (fread(buffer.data(), 1, take, src) != take) {
            *error = "cannot read archive";
            return false;
        }
        if (!writeBytes(out, buffer.data(), take, crc)) {
            *error = "cannot write file";
            return false;
        }
        if (onBytes) onBytes(take);
        remaining -= take;
    }
    return true;
}

bool inflateEntry(FILE* src, uint64_t offset, uint64_t compressedSize,
                  uint64_t size, FILE* out, uLong* crc,
                  const std::function<bool()>& cancelled,
                  const ByteProgress& onBytes, std::string* error) {
    if (size == 0) return true;
    if (fseeko(src, (off_t)offset, SEEK_SET) != 0) {
        *error = "cannot seek archive";
        return false;
    }

    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        *error = "cannot initialise zlib";
        return false;
    }

    std::vector<unsigned char> input(kChunk);
    std::vector<unsigned char> output(kChunk);
    uint64_t remaining = compressedSize;
    uint64_t written = 0;
    bool ok = true;

    while (true) {
        if (cancelled && cancelled()) {
            *error = "cancelled";
            ok = false;
            break;
        }
        if (stream.avail_in == 0) {
            if (remaining == 0) break;
            size_t take = (size_t)std::min<uint64_t>(remaining, input.size());
            if (fread(input.data(), 1, take, src) != take) {
                *error = "cannot read archive";
                ok = false;
                break;
            }
            remaining -= take;
            stream.next_in = input.data();
            stream.avail_in = (uInt)take;
        }

        stream.next_out = output.data();
        stream.avail_out = (uInt)output.size();
        int ret = inflate(&stream, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            *error = "corrupt deflate stream";
            ok = false;
            break;
        }

        size_t produced = output.size() - stream.avail_out;
        if (produced > 0) {
            written += produced;
            if (written > size) {
                *error = "inflated size mismatch";
                ok = false;
                break;
            }
            if (!writeBytes(out, output.data(), produced, crc)) {
                *error = "cannot write file";
                ok = false;
                break;
            }
            if (onBytes) onBytes(produced);
            if (ret == Z_STREAM_END) break;
        } else if (ret == Z_STREAM_END) {
            break;
        } else if (stream.avail_in == 0 && remaining == 0) {
            *error = "truncated deflate stream";
            ok = false;
            break;
        }
    }

    inflateEnd(&stream);
    if (ok && written != size) {
        *error = "inflated size mismatch";
        ok = false;
    }
    return ok;
}

/// Opens the destination, falling back to an ASCII name when FatFs refuses
/// the UTF-8 path. Returns false when neither variant can be created.
FILE* openDestination(const std::string& outputDir,
                      const std::string& relative, std::string* openedPath) {
    std::string path = uniquePath(joinPath(outputDir, relative));
    ensureDir(parentDir(path));
    FILE* file = fopen(path.c_str(), "wb");
    if (file) {
        *openedPath = path;
        return file;
    }

    std::string ascii = asciiRelative(relative);
    if (ascii.empty() || ascii == relative) return nullptr;
    path = uniquePath(joinPath(outputDir, ascii));
    ensureDir(parentDir(path));
    file = fopen(path.c_str(), "wb");
    if (file) *openedPath = path;
    return file;
}

/// Resolves the entry name to a safe relative path. Returns false when the
/// entry should be skipped (directories, macOS metadata, unsafe names).
bool entryPath(const Entry& entry, std::string* relative) {
    std::string decoded = subtitleToUtf8(entry.name);
    if (decoded.compare(0, 10, "__MACOSX/") == 0) return false;
    if (decoded == ".DS_Store" ||
        (decoded.size() > 9 &&
         decoded.compare(decoded.size() - 10, 10, "/.DS_Store") == 0))
        return false;

    bool isDir = false;
    bool unsafe = false;
    *relative = sanitizeRelative(decoded, &isDir, &unsafe);
    if (unsafe || relative->empty() || isDir) return false;
    return true;
}

}  // namespace

ZipExtractResult extractZip(
    const std::string& zipPath, const std::string& outputDir,
    const std::function<bool()>& cancelled,
    const std::function<void(uint64_t, uint64_t, int, int,
                             const std::string&)>& progress) {
    ZipExtractResult result;
    result.outputDir = outputDir;

    FILE* file = fopen(zipPath.c_str(), "rb");
    if (!file) {
        result.error = std::string("cannot open archive: ") + strerror(errno);
        return result;
    }

    uint64_t size = 0;
    if (!fileSize(file, &size) || size < 22) {
        fclose(file);
        result.error = "archive is too small";
        return result;
    }

    // End of central directory record, possibly followed by a comment.
    size_t tailSize = (size_t)std::min<uint64_t>(size, 65557);
    std::string tail(tailSize, '\0');
    if (!readAt(file, size - tailSize, &tail[0], tailSize)) {
        fclose(file);
        result.error = "cannot read archive";
        return result;
    }

    size_t eocd = std::string::npos;
    for (size_t i = tailSize - 22 + 1; i-- > 0;) {
        if (read32((const unsigned char*)tail.data() + i) == kEocdSig) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) {
        fclose(file);
        result.error = "not a zip archive";
        return result;
    }

    uint64_t totalEntries = read16((const unsigned char*)tail.data() + eocd + 10);
    uint64_t centralSize = read32((const unsigned char*)tail.data() + eocd + 12);
    uint64_t centralOffset =
        read32((const unsigned char*)tail.data() + eocd + 16);

    if (totalEntries == 0xFFFF || centralSize == 0xFFFFFFFF ||
        centralOffset == 0xFFFFFFFF) {
        uint64_t eocdPosition = size - tailSize + eocd;
        unsigned char locator[20];
        if (eocdPosition >= 20 &&
            readAt(file, eocdPosition - 20, locator, sizeof(locator)) &&
            read32(locator) == kZip64LocatorSig) {
            unsigned char record[56];
            uint64_t zip64Offset = read64(locator + 8);
            if (readAt(file, zip64Offset, record, sizeof(record)) &&
                read32(record) == kZip64EocdSig) {
                totalEntries = read64(record + 32);
                centralSize = read64(record + 40);
                centralOffset = read64(record + 48);
            }
        }
    }

    if (centralOffset > size || centralSize > size - centralOffset ||
        centralSize > 64ULL * 1024ULL * 1024ULL) {
        fclose(file);
        result.error = "corrupt central directory";
        return result;
    }

    std::string central((size_t)centralSize, '\0');
    if (!readAt(file, centralOffset, &central[0], central.size())) {
        fclose(file);
        result.error = "cannot read central directory";
        return result;
    }

    std::vector<Entry> entries;
    entries.reserve((size_t)std::min<uint64_t>(totalEntries, 4096));
    size_t pos = 0;
    while (pos + 46 <= central.size()) {
        const unsigned char* header =
            (const unsigned char*)central.data() + pos;
        if (read32(header) != kCentralSig) break;

        Entry entry;
        entry.flags = read16(header + 8);
        entry.method = read16(header + 10);
        entry.crc = read32(header + 16);
        entry.compressedSize = read32(header + 20);
        entry.size = read32(header + 24);
        uint16_t nameLen = read16(header + 28);
        uint16_t extraLen = read16(header + 30);
        uint16_t commentLen = read16(header + 32);
        entry.localOffset = read32(header + 42);

        if (pos + 46 + nameLen + extraLen + commentLen > central.size()) break;
        entry.name = central.substr(pos + 46, nameLen);
        parseZip64Extra(central, pos + 46 + nameLen, extraLen, &entry);
        entries.push_back(std::move(entry));

        pos += 46 + (size_t)nameLen + extraLen + commentLen;
    }

    if (entries.empty()) {
        fclose(file);
        result.error = "archive has no entries";
        return result;
    }

    if (!ensureDir(outputDir)) {
        fclose(file);
        result.error = "cannot create output directory";
        return result;
    }

    uint64_t totalBytes = 0;
    int totalFiles = 0;
    for (const Entry& entry : entries) {
        std::string relative;
        if (!entryPath(entry, &relative)) continue;
        totalBytes += entry.size;
        totalFiles++;
    }

    uint64_t doneBytes = 0;
    int doneFiles = 0;
    auto report = [&](const std::string& name) {
        if (progress)
            progress(doneBytes, totalBytes, doneFiles, totalFiles, name);
    };

    bool failed = false;
    for (const Entry& entry : entries) {
        if (failed) break;
        if (cancelled && cancelled()) {
            result.error = "cancelled";
            failed = true;
            break;
        }

        std::string relative;
        if (!entryPath(entry, &relative)) continue;
        report(relative);

        if (entry.flags & 0x0001) {
            result.error = "encrypted archives are not supported";
            failed = true;
            break;
        }
        if (entry.method != 0 && entry.method != 8) {
            result.error = "unsupported compression method for " + relative;
            failed = true;
            break;
        }

        unsigned char local[30];
        if (!readAt(file, entry.localOffset, local, sizeof(local)) ||
            read32(local) != kLocalSig) {
            result.error = "corrupt local header for " + relative;
            failed = true;
            break;
        }
        uint16_t localNameLen = read16(local + 26);
        uint16_t localExtraLen = read16(local + 28);
        uint64_t dataOffset =
            entry.localOffset + 30 + localNameLen + localExtraLen;
        if (dataOffset > size || entry.compressedSize > size - dataOffset) {
            result.error = "invalid entry size for " + relative;
            failed = true;
            break;
        }

        std::string openedPath;
        FILE* out = openDestination(outputDir, relative, &openedPath);
        if (!out) {
            result.error = "cannot create " + relative;
            failed = true;
            break;
        }

        auto onBytes = [&](uint64_t delta) {
            doneBytes += delta;
            report(relative);
        };

        uLong crc = crc32(0L, Z_NULL, 0);
        std::string error;
        bool ok = entry.method == 0
                      ? copyStored(file, dataOffset, entry.compressedSize, out,
                                   &crc, cancelled, onBytes, &error)
                      : inflateEntry(file, dataOffset, entry.compressedSize,
                                     entry.size, out, &crc, cancelled, onBytes,
                                     &error);
        bool flushed = fflush(out) == 0;
        fclose(out);

        if (!ok || !flushed) {
            unlink(openedPath.c_str());
            result.error = error.empty() ? "cannot write " + relative : error;
            failed = true;
            break;
        }
        if (crc != (uLong)entry.crc) {
            unlink(openedPath.c_str());
            result.error = "checksum mismatch for " + relative;
            failed = true;
            break;
        }
        result.files++;
        doneFiles++;
        report(relative);
    }

    fclose(file);
    result.ok = !failed;
    if (failed && result.files == 0) result.outputDir.clear();
    return result;
}

}  // namespace cloud
