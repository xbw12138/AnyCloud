#include "nxkit/http_log.hpp"

namespace nxkit {

HttpLog& HttpLog::instance() {
    static HttpLog log;
    return log;
}

void HttpLog::add(HttpLogEntry entry) {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->bodyBytes += entry.snippet.size();
    this->entries.push_back(std::move(entry));
    // Drop the oldest entries until both the count and the retained body bytes
    // fit; always keep the entry just added, even if it alone is bigger.
    while (this->entries.size() > 1 &&
           (this->entries.size() > kCapacity ||
            this->bodyBytes > kMaxBodyBytes)) {
        this->bodyBytes -= this->entries.front().snippet.size();
        this->entries.pop_front();
    }
}

std::vector<HttpLogEntry> HttpLog::snapshot() const {
    std::lock_guard<std::mutex> lock(this->mutex);
    return std::vector<HttpLogEntry>(this->entries.begin(), this->entries.end());
}

void HttpLog::clear() {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->entries.clear();
    this->bodyBytes = 0;
}

}  // namespace nxkit
