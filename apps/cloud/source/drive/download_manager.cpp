//
// cloud: background file downloads to the local filesystem.
//

#include "cloud/drive/download_manager.hpp"

#include <cpr/async.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <deque>
#include <fstream>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include <curl/curl.h>

#include <borealis/core/application.hpp>
#include <borealis/core/thread.hpp>
#include <nlohmann/json.hpp>

#include "nxkit/http_client.hpp"
#include "nxkit/paths.hpp"

namespace cloud {

namespace {

const char* kHistoryFileName = "downloads.json";

}  // namespace

namespace {

std::string sanitizeName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c < 0x20) {
            out.push_back('_');
        } else if (c == '?' || c == '*' || c == '"' || c == '<' ||
                   c == '>' || c == '|') {
            // Characters FAT/exFAT reject; the SD card would fail to open.
            out.push_back('_');
        } else {
            out.push_back((char)c);
        }
    }
    // FAT also dislikes trailing dots and spaces.
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))
        out.pop_back();
    // Keep well under the 255-byte filesystem component limit.
    if (out.size() > 180) {
        out.resize(180);
        while (!out.empty() && ((unsigned char)out.back() & 0xC0) == 0x80)
            out.pop_back();
        if (!out.empty() && (unsigned char)out.back() >= 0xC0) out.pop_back();
    }
    if (out.empty() || out == "." || out == "..") out = "download";
    return out;
}

/// ASCII-only fallback used when FatFs refuses the original name. Non-ASCII
/// bytes collapse into a single underscore so the extension stays readable.
std::string asciiName(const std::string& name) {
    std::string out;
    bool lastUnderscore = false;
    for (unsigned char c : name) {
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
    if (out.empty() || out == "." || out == "..") out = "download";
    // A fully non-ASCII title collapses into "_.mp4"; give it a real stem.
    size_t dot = out.find_last_of('.');
    std::string stem = dot == std::string::npos ? out : out.substr(0, dot);
    if (stem.find_first_not_of("_-") == std::string::npos)
        out = "download" + (dot == std::string::npos ? "" : out.substr(dot));
    if (out.size() > 80) {
        size_t mark = out.find_last_of('.');
        std::string ext =
            (mark != std::string::npos && out.size() - mark <= 10)
                ? out.substr(mark)
                : std::string();
        out = out.substr(0, 80 - ext.size()) + ext;
    }
    return out;
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

bool pathExists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}

/// Size of a regular file, 0 when missing.
uint64_t fileSize(const std::string& path) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return 0;
    return (uint64_t)info.st_size;
}

bool isDirectory(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::string normalizeDirectory(const std::string& dir) {
    std::string out = dir;
    size_t first = out.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    out.erase(0, first);
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t' ||
                            out.back() == '\r' || out.back() == '\n'))
        out.pop_back();
#ifdef __SWITCH__
    if (out == "sdmc:") out = "sdmc:/";
#endif
    return out;
}

std::string uniquePath(const std::string& path) {
    if (!pathExists(path)) return path;
    size_t slash = path.find_last_of('/');
    std::string dir = slash == std::string::npos ? "" : path.substr(0, slash + 1);
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
    std::string ext = dot == std::string::npos ? "" : name.substr(dot);
    for (int i = 1; i < 10000; i++) {
        std::string candidate =
            dir + stem + " (" + std::to_string(i) + ")" + ext;
        if (!pathExists(candidate)) return candidate;
    }
    return path;
}

void makeDirs(const std::string& path) {
    if (path.empty()) return;
    std::string current;
    size_t start = 0;
    if (path[0] == '/') {
        current = "/";
        start = 1;
    }
    while (start < path.size()) {
        size_t slash = path.find('/', start);
        std::string part = path.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty()) {
            current = current.empty() ? part : joinPath(current, part);
            mkdir(current.c_str(), 0777);
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
}

/// Read/write pipeline used by every download, modelled after libhaze's
/// threaded_file_transfer: the network thread fills a ring of 1 MiB buffers
/// while a writer thread drains them to the SD card. A slow fwrite therefore
/// never stalls the socket, and the card keeps being fed while the next chunk
/// is still in flight.
class TransferPipeline {
public:
    explicit TransferPipeline(FILE* file) : file(file) {
        this->writer = std::thread([this] { this->writerLoop(); });
    }

    /// Never leaves a joinable thread behind, even when the transfer is
    /// abandoned before finish() (cpr throws, early return, ...).
    ~TransferPipeline() {
        {
            std::lock_guard<std::mutex> lock(this->mutex);
            this->stopping = true;
        }
        this->filled.notify_all();
        if (this->writer.joinable()) this->writer.join();
    }

    /// Producer side. Returns false once a write failed, which aborts the
    /// transfer through the curl write callback.
    bool write(const std::string& data) {
        this->pending.append(data);
        if (this->pending.size() < kChunkSize) return true;
        return this->flush();
    }

    /// Producer side, after the last byte: flushes the tail and joins.
    bool finish() {
        bool ok = this->flush();
        {
            std::lock_guard<std::mutex> lock(this->mutex);
            this->stopping = true;
        }
        this->filled.notify_all();
        if (this->writer.joinable()) this->writer.join();
        return ok && !this->failed();
    }

    std::string errorText() {
        std::lock_guard<std::mutex> lock(this->mutex);
        return this->errorMessage;
    }

private:
    static constexpr size_t kChunkSize = 1024 * 1024;
    static constexpr size_t kBuffers = 4;

    bool failed() {
        std::lock_guard<std::mutex> lock(this->mutex);
        return this->error;
    }

    bool flush() {
        if (this->pending.empty()) return !this->failed();
        std::unique_lock<std::mutex> lock(this->mutex);
        this->space.wait(lock, [this] {
            return this->queue.size() < kBuffers || this->stopping || this->error;
        });
        if (this->error) return false;
        std::string chunk;
        chunk.swap(this->pending);
        this->queue.push_back(std::move(chunk));
        lock.unlock();
        this->filled.notify_one();
        return true;
    }

    void writerLoop() {
        std::string chunk;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(this->mutex);
                this->filled.wait(lock, [this] {
                    return !this->queue.empty() || this->stopping || this->error;
                });
                if (this->queue.empty()) {
                    if (this->stopping || this->error) break;
                    continue;
                }
                chunk = std::move(this->queue.front());
                this->queue.pop_front();
            }
            this->space.notify_one();

            if (chunk.empty()) continue;
            if (fwrite(chunk.data(), 1, chunk.size(), this->file) != chunk.size()) {
                std::string reason = strerror(errno);
                {
                    std::lock_guard<std::mutex> lock(this->mutex);
                    this->error = true;
                    this->errorMessage = "write failed: " + reason;
                }
                this->space.notify_all();
                this->filled.notify_all();
                break;
            }
        }
    }

    FILE* file = nullptr;
    std::thread writer;
    std::mutex mutex;
    /// Producer waits here while both buffers are full.
    std::condition_variable space;
    /// Writer waits here while no buffered data is available.
    std::condition_variable filled;
    std::deque<std::string> queue;
    std::string pending;
    bool stopping = false;
    bool error = false;
    std::string errorMessage;
};

// -----------------------------------------------------------------------------
// Parallel range download
// -----------------------------------------------------------------------------

// A single TLS stream from the console tops out around 2 MB/s (TCP window /
// per-flow limit) while the link itself and the SD card are several times
// faster. Files are therefore fetched through a few ranged connections that
// write disjoint regions of one preallocated file; the card still sees large
// sequential 1 MiB writes per connection.
constexpr int kRangeConnections = 4;
constexpr uint64_t kRangeMinTotal = 32ull * 1024 * 1024;
constexpr size_t kRangeChunkSize = 1024 * 1024;

struct RangeResult {
    /// False when the server does not support ranges or the file is small:
    /// the caller then falls back to the plain single-stream download.
    bool attempted = false;
    bool ok = false;
    uint64_t total = 0;
    std::string error;
};

void applyCurlTls(CURL* curl) {
#ifdef __SWITCH__
    // Same as the cpr path below: the console has no usable trust store.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
#else
    // libcurl falls back to the system trust store on desktop.
    (void)curl;
#endif
}

curl_slist* buildCurlHeaders(const std::string& block) {
    curl_slist* list = nullptr;
    size_t start = 0;
    while (start < block.size()) {
        size_t end = block.find('\n', start);
        std::string line = block.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) list = curl_slist_append(list, line.c_str());
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return list;
}

struct RangeContext {
    /// Shared by every connection. The FS service takes the write offset from
    /// the newlib FILE position, so one handle is kept and seek+write pairs are
    /// serialised (the seek itself is only a local offset update).
    FILE* file = nullptr;
    std::mutex* fileMutex = nullptr;
    /// Offset of this range in the file.
    uint64_t start = 0;
    /// Bytes this range is expected to deliver.
    uint64_t expected = 0;
    /// Bytes already written to the file.
    uint64_t flushed = 0;
    /// Bytes accepted from the network (written + buffered tail).
    uint64_t accepted = 0;
    std::string pending;
    std::string error;
    uint64_t total = 0;
    std::atomic<uint64_t>* received = nullptr;
    std::atomic<bool>* abort = nullptr;
    const std::function<bool()>* cancelled = nullptr;
    const std::function<void(uint64_t, uint64_t)>* report = nullptr;
    std::chrono::steady_clock::time_point lastReport;
};

/// Writes the buffered tail at this connection's current offset.
bool flushRange(RangeContext* ctx) {
    if (ctx->pending.empty()) return true;
    size_t size = ctx->pending.size();
    std::lock_guard<std::mutex> lock(*ctx->fileMutex);
    if (fseeko(ctx->file, (off_t)(ctx->start + ctx->flushed), SEEK_SET) != 0) {
        ctx->error = "cannot seek destination file";
        return false;
    }
    if (fwrite(ctx->pending.data(), 1, size, ctx->file) != size) {
        ctx->error = "write failed";
        return false;
    }
    ctx->flushed += size;
    ctx->pending.clear();
    return true;
}

size_t rangeWrite(char* data, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<RangeContext*>(userdata);
    size_t bytes = size * nmemb;
    if (ctx->abort->load() || (*ctx->cancelled)()) {
        ctx->abort->store(true);
        return 0;
    }
    // A body larger than the requested range means the server ignored Range
    // and is sending the whole file; stop before writing it at a wrong offset.
    if (bytes > ctx->expected - ctx->accepted) {
        ctx->abort->store(true);
        return 0;
    }
    ctx->pending.append(data, bytes);
    ctx->accepted += bytes;
    ctx->received->fetch_add(bytes, std::memory_order_relaxed);
    // Batch to 1 MiB so the card sees large sequential writes despite the
    // 16 KiB curl records.
    if (ctx->pending.size() >= kRangeChunkSize && !flushRange(ctx)) {
        ctx->abort->store(true);
        return 0;
    }
    return bytes;
}

int rangeProgress(void* userdata, curl_off_t, curl_off_t, curl_off_t,
                  curl_off_t) {
    auto* ctx = static_cast<RangeContext*>(userdata);
    for (;;) {
        if ((*ctx->cancelled)() || ctx->abort->load()) return 1;
        if (brls::Application::isWindowFocused()) break;
        // System menu in front: stop reading, like the single-stream path.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    auto now = std::chrono::steady_clock::now();
    if (now - ctx->lastReport >= std::chrono::milliseconds(200)) {
        ctx->lastReport = now;
        (*ctx->report)(ctx->received->load(std::memory_order_relaxed),
                       ctx->total);
    }
    return 0;
}

struct RangeProbe {
    uint64_t total = 0;
    bool ranged = false;
};

size_t rangeProbeHeader(char* buffer, size_t size, size_t nmemb,
                        void* userdata) {
    auto* probe = static_cast<RangeProbe*>(userdata);
    size_t bytes = size * nmemb;
    std::string line(buffer, bytes);
    if (line.size() > 14) {
        bool match = true;
        const char* key = "content-range:";
        for (size_t i = 0; i < 14 && match; i++)
            match = ::tolower((unsigned char)line[i]) == key[i];
        if (match) {
            size_t slash = line.find('/');
            if (slash != std::string::npos) {
                long long total = atoll(line.c_str() + slash + 1);
                if (total > 0) {
                    probe->total = (uint64_t)total;
                    probe->ranged = true;
                }
            }
        }
    }
    return bytes;
}

size_t rangeDiscard(char*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}

/// One byte range request: returns empty on success, else a short reason.
std::string runRange(const std::string& url, const std::string& headerBlock,
                     FILE* file, std::mutex* fileMutex, uint64_t start,
                     uint64_t length, uint64_t total,
                     std::atomic<uint64_t>* received,
                     std::atomic<bool>* abort,
                     const std::function<bool()>& cancelled,
                     const std::function<void(uint64_t, uint64_t)>& report) {
    RangeContext ctx;
    ctx.file = file;
    ctx.fileMutex = fileMutex;
    ctx.start = start;
    ctx.expected = length;
    ctx.total = total;
    ctx.received = received;
    ctx.abort = abort;
    ctx.cancelled = &cancelled;
    ctx.report = &report;
    ctx.lastReport = std::chrono::steady_clock::now();

    CURL* curl = curl_easy_init();
    if (!curl) return "cannot init http";
    curl_slist* list = buildCurlHeaders(headerBlock);
    std::string range =
        std::to_string(start) + "-" + std::to_string(start + length - 1);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, rangeWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, rangeProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    applyCurlTls(curl);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);

    bool stored = flushRange(&ctx) && ctx.flushed == length;
    if (!stored)
        return ctx.error.empty() ? "incomplete range download" : ctx.error;
    if (rc != CURLE_OK)
        return std::string("range failed: ") + curl_easy_strerror(rc);
    if (status != 206 && status != 200)
        return "range http " + std::to_string(status);
    return "";
}

/// Asks for `bytes=0-0` first. Returns the total size only when the CDN
/// answers 206 with a Content-Range, i.e. when ranges are really honoured.
bool probeRanges(const std::string& url, const std::string& headerBlock,
                 uint64_t* total) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    curl_slist* list = buildCurlHeaders(headerBlock);
    RangeProbe probe;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, rangeProbeHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &probe);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, rangeDiscard);
    applyCurlTls(curl);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK || status != 206 || !probe.ranged) return false;
    *total = probe.total;
    return true;
}

namespace {

/// Drives whose CDN links support byte ranges are worth opening several
/// connections for. Everything else keeps the plain single stream.
bool supportsRangeDownload(const std::string& url) {
    return url.find("quark.cn") != std::string::npos ||
           url.find("baidupcs.com") != std::string::npos ||
           url.find("pcs.baidu.com") != std::string::npos;
}

}  // namespace

RangeResult downloadRanges(
    const std::string& url, const std::string& headerBlock,
    const std::string& path,
    const std::function<void(uint64_t, uint64_t)>& report,
    const std::function<bool()>& cancelled) {
    RangeResult result;
    if (!supportsRangeDownload(url)) return result;
    const bool baidu = url.find("baidu") != std::string::npos;
    uint64_t total = 0;
    if (!probeRanges(url, headerBlock, &total) || total < kRangeMinTotal)
        return result;  // caller falls back to one stream

    result.attempted = true;
    result.total = total;

    // One handle, preallocated to the full size; the connections then only
    // overwrite their own byte ranges. Reusing the very same handle avoids
    // reopening (and re-locking) the file for every connection.
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) return result;  // fall back to the single stream
    setvbuf(file, nullptr, _IONBF, 0);
    bool reserved = fseeko(file, (off_t)(total - 1), SEEK_SET) == 0 &&
                    fputc(0, file) != EOF && fflush(file) == 0;
    if (!reserved) {
        fclose(file);
        return result;  // fall back to the single stream
    }

    std::atomic<uint64_t> received{0};
    std::atomic<bool> abort{false};
    std::mutex fileMutex;
    uint64_t chunk = total / (uint64_t)kRangeConnections;
    report(0, total);

    std::vector<std::thread> threads;
    std::vector<std::string> errors((size_t)kRangeConnections);
    threads.reserve((size_t)kRangeConnections);
    for (int i = 0; i < kRangeConnections; i++) {
        uint64_t start = chunk * (uint64_t)i;
        uint64_t length =
            i == kRangeConnections - 1 ? total - start : chunk;
        threads.emplace_back([&, i, start, length] {
            errors[(size_t)i] =
                runRange(url, headerBlock, file, &fileMutex, start, length,
                         total, &received, &abort, cancelled, report);
            if (!errors[(size_t)i].empty()) abort.store(true);
        });
    }
    for (auto& thread : threads) thread.join();
    bool flushed = fflush(file) == 0;
    fclose(file);
    if (!flushed) {
        result.error = "write failed";
        return result;
    }

    if (cancelled()) {
        result.error = "cancelled";
        return result;
    }
    // Prefer a real failure over the "incomplete range" that the other
    // connections report after being aborted.
    const std::string* firstError = nullptr;
    for (const auto& error : errors) {
        if (error.empty()) continue;
        if (!firstError) firstError = &error;
        if (error.find("incomplete range") == std::string::npos) {
            firstError = &error;
            break;
        }
    }
    if (firstError) {
        // Baidu sometimes refuses one of the parallel connections; retry the
        // whole file as a single stream rather than failing the task.
        if (baidu) return RangeResult{};
        result.error = *firstError;
        return result;
    }
    result.ok = true;
    report(total, total);
    return result;
}

cpr::Header parseHeaders(const std::string& block) {
    cpr::Header headers;
    size_t start = 0;
    while (start < block.size()) {
        size_t end = block.find('\n', start);
        std::string line = block.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            size_t first = value.find_first_not_of(' ');
            if (first != std::string::npos) value = value.substr(first);
            name.erase(0, name.find_first_not_of(' '));
            name.erase(name.find_last_not_of(' ') + 1);
            if (!name.empty()) headers[name] = value;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return headers;
}

}  // namespace

DownloadManager& DownloadManager::instance() {
    static DownloadManager manager;
    return manager;
}

DownloadManager::DownloadManager() { this->loadHistory(); }

void DownloadManager::loadHistory() {
    std::ifstream in(nxkit::dataPath(kHistoryFileName));
    if (!in) return;

    nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
    if (root.is_discarded() || !root.is_array()) return;

    std::lock_guard<std::mutex> lock(this->mutex);
    for (const auto& item : root) {
        if (!item.is_object()) continue;

        auto state = std::make_shared<TaskState>();
        state->name = item.value("name", std::string());
        state->provider = item.value("provider", std::string());
        state->directory = item.value("directory", std::string());
        state->path = item.value("path", std::string());
        state->error = item.value("error", std::string());
        state->kind = item.value("kind", 0) == 1 ? TransferKind::Upload
                                                 : TransferKind::Download;
        state->localPath = item.value("local_path", std::string());
        state->parentId = item.value("parent_id", std::string());
        if (state->name.empty()) continue;

        int raw = item.value("state", (int)DownloadState::Failed);
        if (raw < (int)DownloadState::Queued ||
            raw > (int)DownloadState::Cancelled)
            raw = (int)DownloadState::Failed;
        state->state = (DownloadState)raw;
        state->received = item.value("received", (uint64_t)0);
        state->total = item.value("total", (uint64_t)0);
        // Restored records have no provider/file handle, so they can only be
        // inspected and deleted.
        state->cancelled.store(true);
        state->id = this->nextId++;
        this->tasks.push_back(state);
    }
}

void DownloadManager::saveHistory() {
    constexpr size_t kMaxRecords = 200;

    nlohmann::json root = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        // Newest first while collecting, then flip back: keeps the file
        // bounded without keeping stale entries forever.
        size_t kept = 0;
        for (auto it = this->tasks.rbegin();
             it != this->tasks.rend() && kept < kMaxRecords; ++it, ++kept) {
            const auto& task = *it;
            nlohmann::json item;
            item["name"] = task->name;
            item["provider"] = task->provider;
            item["directory"] = task->directory;
            item["path"] = task->path;
            item["kind"] = task->kind == TransferKind::Upload ? 1 : 0;
            item["local_path"] = task->localPath;
            item["parent_id"] = task->parentId;
            item["received"] = task->received;
            item["total"] = task->total;
            if (task->state == DownloadState::Queued ||
                task->state == DownloadState::Running) {
                // Cannot resume after a restart: remember it as interrupted.
                item["state"] = (int)DownloadState::Failed;
                item["error"] = "interrupted";
            } else {
                item["state"] = (int)task->state;
                item["error"] = task->error;
            }
            root.push_back(std::move(item));
        }
    }
    std::reverse(root.begin(), root.end());

    nxkit::ensureDataDirectory();
    std::ofstream out(nxkit::dataPath(kHistoryFileName), std::ios::trunc);
    if (!out) return;
    out << root.dump();
}

uint64_t DownloadManager::start(std::shared_ptr<DriveProvider> provider,
                                const DriveFile& file,
                                const std::string& directory) {
    auto state = std::make_shared<TaskState>();
    state->name = file.name;
    state->provider = provider ? provider->name() : std::string();
    state->directory = directory;
    state->providerRef = provider;
    state->file = file;
    {
        std::lock_guard<std::mutex> lock(mutex);
        state->id = nextId++;
        tasks.push_back(state);
    }

    this->launch(state);
    // Remember the task right away so an app exit mid-download still leaves a
    // visible history entry.
    this->saveHistory();
    return state->id;
}

uint64_t DownloadManager::upload(std::shared_ptr<DriveProvider> provider,
                                 const std::string& parentId,
                                 const std::string& localPath,
                                 const std::string& name,
                                 std::function<void(bool)> onDone) {
    auto state = std::make_shared<TaskState>();
    state->kind = TransferKind::Upload;
    state->name = name;
    state->provider = provider ? provider->name() : std::string();
    state->parentId = parentId;
    state->localPath = localPath;
    state->providerRef = provider;
    state->onUploadDone = std::move(onDone);
    {
        std::lock_guard<std::mutex> lock(mutex);
        state->id = nextId++;
        tasks.push_back(state);
    }

    this->launchUpload(state);
    this->saveHistory();
    return state->id;
}

void DownloadManager::launchUpload(const std::shared_ptr<TaskState>& state) {
    auto provider = state->providerRef;
    if (!provider) {
        this->update(state, [](TaskState& task) {
            task.state = DownloadState::Failed;
            task.error = "missing drive provider";
        });
        return;
    }

    struct stat info;
    if (stat(state->localPath.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        this->update(state, [](TaskState& task) {
            task.state = DownloadState::Failed;
            task.error = "cloud/upload/file_missing";
        });
        return;
    }

    const std::string name = state->name;
    const std::string localPath = state->localPath;
    const std::string parentId = state->parentId;
    {
        std::lock_guard<std::mutex> lock(mutex);
        state->total = (uint64_t)info.st_size;
        state->received = 0;
        state->error.clear();
        state->state = DownloadState::Running;
    }

    auto last = std::make_shared<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::now());
    provider->uploadFile(
        parentId, name, localPath,
        [state, last](uint64_t sent, uint64_t total) {
            // The providers report per chunk; the list only refreshes twice a
            // second, so drop the rest instead of hammering the mutex.
            auto now = std::chrono::steady_clock::now();
            if (now - *last < std::chrono::milliseconds(200)) return;
            *last = now;
            DownloadManager::instance().update(
                state, [&](TaskState& task) {
                    if (total > 0) task.total = total;
                    task.received = sent;
                });
        },
        [state](const std::string& error) {
            DownloadManager::instance().update(state, [&](TaskState& task) {
                if (state->cancelled.load()) {
                    task.state = DownloadState::Cancelled;
                    return;
                }
                if (!error.empty()) {
                    task.state = DownloadState::Failed;
                    task.error = error;
                    return;
                }
                task.received = task.total;
                task.state = DownloadState::Done;
            });
            if (state->onUploadDone) {
                auto done = state->onUploadDone;
                state->onUploadDone = nullptr;
                const bool ok = error.empty();
                brls::sync([done, ok] { done(ok); });
            }
        });
}

void DownloadManager::launch(const std::shared_ptr<TaskState>& state) {
    auto provider = state->providerRef;
    if (!provider) {
        this->update(state, [](TaskState& task) {
            task.state = DownloadState::Failed;
            task.error = "missing drive provider";
        });
        return;
    }

    provider->downloadInfo(
        state->file.id,
        [state, provider, name = state->name,
         directory = state->directory](const std::string& url,
                                       const std::string& headers,
                                       const std::string& error) {
            // Cancelled while the provider was still resolving the url.
            if (state->cancelled.load()) {
                DownloadManager::instance().update(
                    state, [](TaskState& task) {
                        task.state = DownloadState::Cancelled;
                    });
                return;
            }
            if (!error.empty() || url.empty()) {
                DownloadManager::instance().update(
                    state, [&](TaskState& task) {
                        task.state = DownloadState::Failed;
                        task.error = error.empty() ? "empty download url" : error;
                    });
                return;
            }

            std::string dir = normalizeDirectory(directory);
            if (dir.empty()) {
                DownloadManager::instance().update(
                    state, [](TaskState& task) {
                        task.state = DownloadState::Failed;
                        task.error = "no download folder selected";
                    });
                return;
            }
            makeDirs(dir);
            if (!isDirectory(dir)) {
                DownloadManager::instance().update(
                    state, [&](TaskState& task) {
                        task.state = DownloadState::Failed;
                        task.error = "cannot access download folder: " + dir;
                    });
                return;
            }
            std::string path = uniquePath(joinPath(dir, sanitizeName(name)));
            {
                std::lock_guard<std::mutex> lock(
                    DownloadManager::instance().mutex);
                state->path = path;
                state->state = DownloadState::Running;
            }

            cpr::async([state, url, headers, path, dir, name] {
                std::string dest = path;
                FILE* file = fopen(dest.c_str(), "wb");
                if (!file) {
                    int openError = errno;
                    // FatFs on Switch reports ENOENT for names it cannot
                    // encode (non-ASCII, reserved chars). Probe the folder to
                    // tell a bad name apart from an unwritable directory.
                    std::string probePath = joinPath(dir, "cloud_probe.tmp");
                    FILE* probeFile = fopen(probePath.c_str(), "wb");
                    if (!probeFile) {
                        std::string reason = strerror(errno);
                        DownloadManager::instance().update(
                            state, [&](TaskState& task) {
                                task.state = DownloadState::Failed;
                                task.error = "cannot write to download folder: " +
                                             dir + " (" + reason + ")";
                            });
                        return;
                    }
                    fclose(probeFile);
                    unlink(probePath.c_str());

                    dest = uniquePath(joinPath(dir, asciiName(name)));
                    DownloadManager::instance().update(
                        state, [&](TaskState& task) { task.path = dest; });
                    file = fopen(dest.c_str(), "wb");
                    if (!file) {
                        std::string reason = strerror(openError);
                        DownloadManager::instance().update(
                            state, [&](TaskState& task) {
                                task.state = DownloadState::Failed;
                                task.error = "cannot open destination file: " +
                                             dest + " (" + reason + ")";
                            });
                        return;
                    }
                }

                fclose(file);

                bool finished = false;
                std::string failure;

                auto report = [state](uint64_t received, uint64_t total) {
                    DownloadManager::instance().update(
                        state, [&](TaskState& task) {
                            if (total > 0) task.total = total;
                            task.received = received;
                        });
                };

                if (!state->cancelled.load()) {
                    DownloadManager::instance().update(
                        state, [](TaskState& task) { task.received = 0; });

                    // Large files go through several ranged streams; the plain
                    // download below is the fallback when Range is unsupported.
                    RangeResult ranges = downloadRanges(
                        url, headers, dest, report,
                        [state] { return state->cancelled.load(); });
                    if (ranges.attempted) {
                        finished = ranges.ok;
                        if (!finished)
                            failure = ranges.error.empty() ? "download failed"
                                                           : ranges.error;
                    } else {
                        FILE* target = fopen(dest.c_str(), "wb");
                        if (!target) {
                            failure = std::string("cannot open destination "
                                                  "file: ") +
                                      strerror(errno);
                        } else {
                            cpr::Header baseHeaders = parseHeaders(headers);

                            // Network reads and SD writes run in parallel (see
                            // TransferPipeline); the writer is joined before
                            // fclose().
                            auto pipeline =
                                std::make_shared<TransferPipeline>(target);

                            cpr::Session session;
                            session.SetUrl(cpr::Url{url});
                            session.SetOption(baseHeaders);
                            session.SetOption(cpr::ConnectTimeout{20000});
                            session.SetOption(cpr::Redirect{true});
                            nxkit::HttpClient::applyCaBundle(session);
#ifdef __SWITCH__
                            session.SetOption(cpr::VerifySsl{false});
#endif
                            session.SetWriteCallback(cpr::WriteCallback(
                                [pipeline, state](std::string data,
                                                  intptr_t) -> bool {
                                    if (state->cancelled.load()) return false;
                                    if (data.empty()) return true;
                                    return pipeline->write(data);
                                }));
                            session.SetProgressCallback(cpr::ProgressCallback(
                                [state](cpr::cpr_pf_arg_t total,
                                        cpr::cpr_pf_arg_t now, cpr::cpr_pf_arg_t,
                                        cpr::cpr_pf_arg_t, intptr_t) -> bool {
                                    // While the system menu is in front, stop
                                    // reading so neither the network nor the SD
                                    // card competes with the console's UI; the
                                    // transfer continues once the app regains
                                    // focus.
                                    while (!brls::Application::isWindowFocused() &&
                                           !state->cancelled.load())
                                        std::this_thread::sleep_for(
                                            std::chrono::milliseconds(50));
                                    DownloadManager::instance().update(
                                        state, [&](TaskState& task) {
                                            if (total > 0)
                                                task.total = (uint64_t)total;
                                            task.received = (uint64_t)now;
                                        });
                                    return !state->cancelled.load();
                                }));

                            cpr::Response response = session.Get();
                            bool written = pipeline->finish();
                            fclose(target);

                            if (!written) {
                                // Writing to the card failed; the socket was
                                // aborted, so report the card error instead of a
                                // curl write error.
                                failure = pipeline->errorText();
                                if (failure.empty())
                                    failure = "download write failed";
                            } else if (!response.error.message.empty() ||
                                       (response.status_code != 200 &&
                                        response.status_code != 206)) {
                                failure = response.error.message;
                                if (failure.empty())
                                    failure = "http " +
                                              std::to_string(
                                                  response.status_code);
                            } else {
                                finished = true;
                            }
                        }
                    }
                }

                if (state->cancelled.load()) {
                    unlink(dest.c_str());
                    DownloadManager::instance().update(
                        state, [](TaskState& task) {
                            task.state = DownloadState::Cancelled;
                        });
                } else if (!finished) {
                    unlink(dest.c_str());
                    DownloadManager::instance().update(
                        state, [&](TaskState& task) {
                            task.state = DownloadState::Failed;
                            task.error = failure.empty() ? "download failed"
                                                         : failure;
                        });
                } else {
                    DownloadManager::instance().update(
                        state, [&](TaskState& task) {
                            uint64_t size = fileSize(dest);
                            // A transfer that ends early must not pass as done.
                            if (task.total > 0 && size + 1024 < task.total) {
                                task.received = size;
                                task.state = DownloadState::Failed;
                                task.error = "incomplete download";
                            } else {
                                task.received = size;
                                task.total = size;
                                task.state = DownloadState::Done;
                            }
                        });
                }
            });
        });
}

void DownloadManager::retry(uint64_t id) {
    std::shared_ptr<TaskState> target;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& task : tasks) {
            if (task->id != id) continue;
            if (!task->providerRef) return;  // restored history: not retryable
            if (task->state == DownloadState::Queued ||
                task->state == DownloadState::Running)
                return;
            task->cancelled.store(false);
            task->received = 0;
            task->total = 0;
            task->path.clear();
            task->error.clear();
            task->state = DownloadState::Queued;
            target = task;
            break;
        }
    }
    if (target) {
        if (target->kind == TransferKind::Upload)
            this->launchUpload(target);
        else
            this->launch(target);
    }
}

void DownloadManager::remove(uint64_t id) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = tasks.begin(); it != tasks.end(); ++it) {
            if ((*it)->id != id) continue;
            if ((*it)->state == DownloadState::Queued ||
                (*it)->state == DownloadState::Running) {
                (*it)->cancelled.store(true);
                if ((*it)->state == DownloadState::Queued)
                    (*it)->state = DownloadState::Cancelled;
            }
            tasks.erase(it);
            break;
        }
    }
    this->saveHistory();
}

void DownloadManager::cancel(uint64_t id) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& task : tasks) {
            if (task->id != id) continue;
            if (task->state == DownloadState::Done ||
                task->state == DownloadState::Failed ||
                task->state == DownloadState::Cancelled)
                return;
            task->cancelled.store(true);
            // Persist right away, otherwise an exit before the worker notices
            // would leave a stale queued record behind.
            task->state = DownloadState::Cancelled;
            changed = true;
            break;
        }
    }
    if (changed) this->saveHistory();
}

void DownloadManager::clearFinished() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        tasks.erase(std::remove_if(tasks.begin(), tasks.end(),
                                   [](const std::shared_ptr<TaskState>& task) {
                                       return task->state == DownloadState::Done ||
                                              task->state == DownloadState::Failed ||
                                              task->state ==
                                                  DownloadState::Cancelled;
                                   }),
                    tasks.end());
    }
    this->saveHistory();
}

std::vector<DownloadTask> DownloadManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<DownloadTask> result;
    result.reserve(tasks.size());
    // Newest first so the task just queued sits at the top of the list.
    for (auto it = tasks.rbegin(); it != tasks.rend(); ++it) {
        const auto& task = *it;
        DownloadTask info;
        info.id = task->id;
        info.name = task->name;
        info.provider = task->provider;
        info.path = task->kind == TransferKind::Upload ? task->localPath
                                                       : task->path;
        info.received = task->received;
        info.total = task->total;
        info.state = task->state;
        info.error = task->error;
        info.kind = task->kind;
        info.retryable = task->providerRef != nullptr;
        result.push_back(std::move(info));
    }
    return result;
}

void DownloadManager::update(const std::shared_ptr<TaskState>& state,
                             std::function<void(TaskState&)> mutate) {
    bool finished = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        mutate(*state);
        finished = state->state == DownloadState::Done ||
                   state->state == DownloadState::Failed ||
                   state->state == DownloadState::Cancelled;
    }
    // Persist history whenever a task reaches a final state.
    if (finished) this->saveHistory();
}

}  // namespace cloud
