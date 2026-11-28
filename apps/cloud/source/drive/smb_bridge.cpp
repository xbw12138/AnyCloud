//
// cloud: local http bridge in front of an SMB share.
//

#include "cloud/drive/smb_bridge.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include <pthread.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace cloud {

namespace {

constexpr int kFirstPort = 19080;
constexpr int kPortTries = 12;
constexpr size_t kChunkSize = 256 * 1024;
constexpr size_t kMaxHeaderBytes = 16 * 1024;
/// One worker can sit in a long video stream, so keep a couple spare for the
/// image and document viewers.
constexpr size_t kWorkerCount = 3;
constexpr size_t kMaxQueued = 6;
/// The devkitPro default is 128 KiB, which libsmb2's read path can scrape.
constexpr size_t kThreadStackBytes = 512 * 1024;
/// A client that connects and then says nothing must not pin a worker.
constexpr int kClientTimeoutSeconds = 15;

/// libnx's pthread_detach is a stub that always fails with ENOSYS, so
/// std::thread::detach() throws std::system_error and the temporary thread
/// object then terminates the process while still joinable. Long lived bridge
/// threads are therefore created here instead, and simply live until exit.
bool spawnThread(void* (*entry)(void*), void* arg);

/// True when nothing listens on `port` on loopback. Binding only succeeds on a
/// free port, which is how start() notices that the listener disappeared.
bool portIsFree(int port) {
    int fd = (int)::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool free =
        ::bind(fd, (struct sockaddr*)&address, sizeof(address)) == 0;
    ::close(fd);
    return free;
}

/// Temporary on-device diagnostics, same file family as the QR view's log.
///
/// Raw file descriptors instead of stdio: this runs on the listener and on the
/// worker threads, and every fopen() touches newlib's shared FILE table.
void logDebug(const std::string& line) {
#ifdef __SWITCH__
    const char* path = "sdmc:/switch/nxkit/smb_debug.log";
#else
    const char* path = "/tmp/smb_debug.log";
#endif
    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);
    int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    std::string text = line + "\n";
    ::write(fd, text.data(), text.size());
    ::close(fd);
}

bool spawnThread(void* (*entry)(void*), void* arg) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return false;
    pthread_attr_setstacksize(&attr, kThreadStackBytes);
    pthread_t thread;
    int rc = pthread_create(&thread, &attr, entry, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        logDebug("bridge: pthread_create failed rc=" + std::to_string(rc));
        return false;
    }
    return true;
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

std::string trim(const std::string& text) {
    size_t begin = 0;
    while (begin < text.size() &&
           std::isspace((unsigned char)text[begin]) != 0)
        begin++;
    size_t end = text.size();
    while (end > begin && std::isspace((unsigned char)text[end - 1]) != 0) end--;
    return text.substr(begin, end - begin);
}

bool sendAll(int fd, const void* data, size_t length) {
    const char* cursor = static_cast<const char*>(data);
    while (length > 0) {
        int sent = (int)::send(fd, cursor, length, 0);
        if (sent <= 0) return false;
        cursor += sent;
        length -= (size_t)sent;
    }
    return true;
}

bool sendText(int fd, const std::string& text) {
    return sendAll(fd, text.data(), text.size());
}

void replyError(int fd, int status, const std::string& message) {
    const char* reason = "Error";
    switch (status) {
        case 400: reason = "Bad Request"; break;
        case 404: reason = "Not Found"; break;
        case 405: reason = "Method Not Allowed"; break;
        case 416: reason = "Range Not Satisfiable"; break;
        case 502: reason = "Bad Gateway"; break;
        case 503: reason = "Service Unavailable"; break;
        default: break;
    }
    std::string body = message + "\n";
    sendText(fd, "HTTP/1.1 " + std::to_string(status) + " " + reason +
                     "\r\n"
                     "Content-Type: text/plain; charset=utf-8\r\n"
                     "Content-Length: " +
                     std::to_string(body.size()) +
                     "\r\n"
                     "Connection: close\r\n\r\n" +
                     body);
}

struct Request {
    std::string method;
    std::string path;
    std::string query;
    std::string range;
};

/// Reads one request head. The body is never used, so the socket is left at
/// the end of the headers.
bool readRequest(int fd, Request& request) {
    std::string head;
    char chunk[2048];
    size_t headEnd = std::string::npos;
    while (headEnd == std::string::npos) {
        if (head.size() > kMaxHeaderBytes) return false;
        int count = (int)::recv(fd, chunk, sizeof(chunk), 0);
        if (count <= 0) return false;
        head.append(chunk, (size_t)count);
        headEnd = head.find("\r\n\r\n");
    }

    std::string text = head.substr(0, headEnd);
    size_t lineEnd = text.find("\r\n");
    std::string line = text.substr(0, lineEnd);
    size_t first = line.find(' ');
    size_t second = first == std::string::npos
                        ? std::string::npos
                        : line.find(' ', first + 1);
    if (first == std::string::npos || second == std::string::npos) return false;

    request.method = line.substr(0, first);
    std::string target = line.substr(first + 1, second - first - 1);
    size_t queryStart = target.find('?');
    request.path = queryStart == std::string::npos
                       ? target
                       : target.substr(0, queryStart);
    if (queryStart != std::string::npos)
        request.query = target.substr(queryStart + 1);

    while (lineEnd != std::string::npos) {
        size_t start = lineEnd + 2;
        lineEnd = text.find("\r\n", start);
        std::string header =
            text.substr(start, lineEnd == std::string::npos
                                   ? std::string::npos
                                   : lineEnd - start);
        size_t colon = header.find(':');
        if (colon == std::string::npos) continue;
        if (toLower(trim(header.substr(0, colon))) == "range")
            request.range = trim(header.substr(colon + 1));
    }
    return true;
}

/// Value of one query parameter, percent decoded (the bridge encoder only
/// escapes reserved bytes, so "+" is a literal plus and not a space).
std::string queryParam(const std::string& query, const char* name) {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t end = query.find('&', pos);
        std::string pair =
            query.substr(pos, end == std::string::npos ? std::string::npos
                                                       : end - pos);
        size_t equal = pair.find('=');
        if (equal != std::string::npos && pair.substr(0, equal) == name)
            return smbDecodePath(pair.substr(equal + 1));
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return std::string();
}

bool parseRange(const std::string& header, uint64_t size, uint64_t& start,
                uint64_t& end) {
    if (header.compare(0, 6, "bytes=") != 0) return false;
    std::string spec = header.substr(6);
    size_t comma = spec.find(',');
    if (comma != std::string::npos) spec = spec.substr(0, comma);
    size_t dash = spec.find('-');
    if (dash == std::string::npos) return false;
    std::string first = spec.substr(0, dash);
    std::string second = spec.substr(dash + 1);
    auto dropSpaces = [](std::string& text) {
        text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
                       return std::isspace(c) != 0;
                   }),
                   text.end());
    };
    dropSpaces(first);
    dropSpaces(second);

    if (first.empty()) {
        // Suffix range: the last N bytes.
        uint64_t count = strtoull(second.c_str(), nullptr, 10);
        if (count == 0 || size == 0) return false;
        if (count > size) count = size;
        start = size - count;
        end = size - 1;
        return true;
    }
    start = strtoull(first.c_str(), nullptr, 10);
    end = second.empty() ? size - 1 : strtoull(second.c_str(), nullptr, 10);
    if (size == 0 || start >= size) return false;
    if (end >= size) end = size - 1;
    return start <= end;
}

void streamFile(int fd, SmbSession& session, smb2fh* file, uint64_t start,
                uint64_t end) {
    std::vector<char> buffer(kChunkSize);
    uint64_t offset = start;
    while (offset <= end) {
        size_t want = (size_t)std::min<uint64_t>(kChunkSize, end - offset + 1);
        std::string error;
        int count = session.readAt(file, offset, buffer.data(), want, error);
        if (count <= 0) {
            logDebug("bridge: read failed at " + std::to_string(offset) + ": " +
                     error);
            return;
        }
        if (!sendAll(fd, buffer.data(), (size_t)count)) {
            logDebug("bridge: client closed at " + std::to_string(offset));
            return;
        }
        offset += (uint64_t)count;
    }
    logDebug("bridge: stream finished at " + std::to_string(offset));
}

void serveClient(int fd) {
    logDebug("bridge: client accepted");
    Request request;
    if (!readRequest(fd, request)) {
        logDebug("bridge: request head unreadable");
        return;
    }
    if (request.method != "GET" && request.method != "HEAD") {
        replyError(fd, 405, "method not allowed");
        return;
    }

    std::string sharePath = queryParam(request.query, "p");
    logDebug("bridge: " + request.method + " path='" + sharePath + "'");
    if (sharePath.empty()) {
        replyError(fd, 400, "missing path");
        return;
    }

    SmbBridge& bridge = SmbBridge::instance();
    SmbConfig config = bridge.config();
    if (!config.valid()) {
        replyError(fd, 503, "smb share is not configured");
        return;
    }

    logDebug("bridge: connecting to share");
    SmbSession session(config);
    std::string error;
    if (!session.connect(error)) {
        logDebug("bridge: share connect failed: " + error);
        replyError(fd, 502, error);
        return;
    }

    uint64_t size = 0;
    smb2fh* file = session.openRead(sharePath, size, error);
    if (!file) {
        logDebug("bridge: open failed: " + error);
        replyError(fd, 404, error);
        return;
    }

    uint64_t start = 0;
    uint64_t end = size > 0 ? size - 1 : 0;
    bool partial = false;
    if (!request.range.empty()) {
        if (!parseRange(request.range, size, start, end)) {
            session.closeFile(file);
            sendText(fd, "HTTP/1.1 416 Range Not Satisfiable\r\n"
                         "Content-Range: bytes */" +
                             std::to_string(size) +
                             "\r\n"
                             "Content-Length: 0\r\n"
                             "Connection: close\r\n\r\n");
            return;
        }
        partial = true;
    }

    uint64_t length = size == 0 ? 0 : end - start + 1;
    std::string headers;
    if (partial) {
        headers = "HTTP/1.1 206 Partial Content\r\n"
                  "Content-Type: application/octet-stream\r\n"
                  "Accept-Ranges: bytes\r\n"
                  "Content-Length: " +
                  std::to_string(length) +
                  "\r\n"
                  "Content-Range: bytes " +
                  std::to_string(start) + "-" + std::to_string(end) + "/" +
                  std::to_string(size) +
                  "\r\n"
                  "Connection: close\r\n\r\n";
    } else {
        headers = "HTTP/1.1 200 OK\r\n"
                  "Content-Type: application/octet-stream\r\n"
                  "Accept-Ranges: bytes\r\n"
                  "Content-Length: " +
                  std::to_string(length) +
                  "\r\n"
                  "Connection: close\r\n\r\n";
    }
    logDebug("bridge: " + request.method + " " + sharePath + " range='" +
             request.range + "' size=" + std::to_string(size) + " -> " +
             std::to_string(length) + " from " + std::to_string(start));

    bool sent = sendText(fd, headers);
    if (sent && request.method == "GET" && length > 0)
        streamFile(fd, session, file, start, end);
    session.closeFile(file);
}

}  // namespace

SmbBridge& SmbBridge::instance() {
    static SmbBridge bridge;
    return bridge;
}

void SmbBridge::setConfig(const SmbConfig& config) {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->credentials = config;
}

SmbConfig SmbBridge::config() const {
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->credentials;
}

std::string SmbBridge::urlFor(const std::string& path) {
    std::string base = this->start();
    if (base.empty()) return std::string();
    return base + "/stream?p=" + smbEncodePath(smbNormalize(path));
}

std::string SmbBridge::start() {
    std::unique_lock<std::mutex> lock(this->mutex);
    if (this->starting) {
        if (!this->finished) {
            this->ready.wait_for(lock, std::chrono::seconds(3),
                                 [this] { return this->finished; });
            return this->base;
        }
        // The listener can be torn down while the app keeps running (the
        // players and libsmb2 open many sockets, and a stray close releases
        // our descriptor). The cached url then answers "connection refused"
        // forever, so re-check that the port is actually still bound.
        const int port = this->listenPort;
        lock.unlock();
        const bool alive = port > 0 && !portIsFree(port);
        lock.lock();
        if (alive) return this->base;
        logDebug("bridge: listener on " + std::to_string(port) +
                 " is gone, restarting");
        this->starting = false;
        this->finished = false;
        this->base.clear();
        this->listenPort = 0;
    }
    this->starting = true;
    lock.unlock();
    logDebug("bridge: starting listener");
    if (!spawnThread(&SmbBridge::listenEntry, this)) {
        lock.lock();
        this->starting = false;
        this->finished = true;
        this->base.clear();
        return std::string();
    }

    lock.lock();
    this->ready.wait_for(lock, std::chrono::seconds(3),
                         [this] { return this->finished; });
    return this->base;
}

void* SmbBridge::listenEntry(void* arg) {
    SmbBridge* bridge = static_cast<SmbBridge*>(arg);
    // Anything escaping a pthread entry point ends in std::terminate.
    try {
        bridge->run();
    } catch (...) {
        logDebug("bridge: listener failed");
    }
    return nullptr;
}

void* SmbBridge::workEntry(void* arg) {
    SmbBridge* bridge = static_cast<SmbBridge*>(arg);
    try {
        bridge->work();
    } catch (...) {
        logDebug("bridge: worker failed");
    }
    return nullptr;
}

void SmbBridge::work() {
    while (true) {
        int client = -1;
        {
            std::unique_lock<std::mutex> lock(this->queueMutex);
            this->queueReady.wait(lock,
                                  [this] { return !this->pending.empty(); });
            client = this->pending.front();
            this->pending.erase(this->pending.begin());
        }
        try {
            serveClient(client);
        } catch (...) {
            // A broken share must not take a worker down.
        }
        ::close(client);
    }
}

void SmbBridge::run() {
    int listener = -1;
    int port = 0;
    for (int i = 0; i < kPortTries && listener < 0; i++) {
        int candidate = (int)::socket(AF_INET, SOCK_STREAM, 0);
        if (candidate < 0) break;

        int reuse = 1;
        ::setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons((uint16_t)(kFirstPort + i));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(candidate, (struct sockaddr*)&address, sizeof(address)) != 0 ||
            ::listen(candidate, 8) != 0) {
            ::close(candidate);
            continue;
        }
        listener = candidate;
        port = kFirstPort + i;
    }

    {
        std::lock_guard<std::mutex> lock(this->mutex);
        this->base = listener >= 0
                         ? "http://127.0.0.1:" + std::to_string(port)
                         : std::string();
        this->listenPort = listener >= 0 ? port : 0;
        this->finished = true;
    }
    logDebug(std::string("bridge: listen ") +
             (listener >= 0 ? "ok " + this->base : std::string("failed")));
    this->ready.notify_all();

    if (listener < 0) return;

    for (size_t i = 0; i < kWorkerCount; i++)
        spawnThread(&SmbBridge::workEntry, this);

    // Runs until the process exits; the app owns this thread's lifetime.
    while (true) {
        int client = (int)::accept(listener, nullptr, nullptr);
        if (client < 0) {
            ::usleep(10 * 1000);
            continue;
        }
#ifdef SO_NOSIGPIPE
        int noSignal = 1;
        ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &noSignal,
                     sizeof(noSignal));
#endif
        struct timeval timeout;
        timeout.tv_sec = kClientTimeoutSeconds;
        timeout.tv_usec = 0;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        bool queued = false;
        {
            std::lock_guard<std::mutex> guard(this->queueMutex);
            if (this->pending.size() < kMaxQueued) {
                this->pending.push_back(client);
                queued = true;
            }
        }
        if (!queued) {
            replyError(client, 503, "too many streams");
            ::close(client);
            continue;
        }
        this->queueReady.notify_one();
    }
}

}  // namespace cloud
