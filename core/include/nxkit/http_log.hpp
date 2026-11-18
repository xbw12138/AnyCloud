//
// nxkit: process-wide network log.
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace nxkit {

/// One completed HTTP transaction, or a cover cache decision. Only text/JSON
/// bodies are retained, in full, so the detail page can show them uncut; the
/// ring drops the oldest entries when the retained bodies grow past its
/// memory budget.
struct HttpLogEntry {
    int64_t time = 0;    // unix seconds
    std::string method;  // GET / POST / CACHE
    std::string url;
    long status = 0;
    int code = 0;  // curl error code, 0 when the request completed
    int durationMs = 0;
    size_t bytes = 0;
    bool ok = false;
    std::string error;    // "HTTP 401" or the transport error
    std::string snippet;  // text/JSON body (full)
    std::string note;     // content type, cache result, ...
};

/// Ring buffer of the most recent requests. Written from worker threads,
/// read (snapshot) from the UI thread.
class HttpLog {
public:
    static HttpLog& instance();

    void add(HttpLogEntry entry);
    std::vector<HttpLogEntry> snapshot() const;
    void clear();

private:
    static const size_t kCapacity     = 400;
    static const size_t kMaxBodyBytes = 3 * 1024 * 1024;

    mutable std::mutex mutex;
    std::deque<HttpLogEntry> entries;
    size_t bodyBytes = 0;
};

}  // namespace nxkit
