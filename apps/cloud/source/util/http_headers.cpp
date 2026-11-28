//
// cloud: provider header block helpers (players, viewers, downloads).
//

#include "cloud/util/http_headers.hpp"

#ifdef NXKIT_WITH_MPV
#include "nxkit/media/mpv_core.hpp"
#endif

namespace cloud {

std::string headerValue(const std::string& headers, const std::string& name) {
    size_t start = 0;
    while (start <= headers.size()) {
        size_t end = headers.find('\n', start);
        std::string line = headers.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > name.size() &&
            line.compare(0, name.size(), name) == 0 &&
            line[name.size()] == ':') {
            size_t value = line.find_first_not_of(" \t", name.size() + 1);
            return value == std::string::npos ? "" : line.substr(value);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return "";
}

std::string headersWithout(const std::string& headers,
                           const std::string& name) {
    std::string out;
    size_t start = 0;
    while (start <= headers.size()) {
        size_t end = headers.find('\n', start);
        std::string line = headers.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        std::string trimmed = line;
        if (!trimmed.empty() && trimmed.back() == '\r') trimmed.pop_back();
        bool drop = trimmed.size() > name.size() &&
                    trimmed.compare(0, name.size(), name) == 0 &&
                    trimmed[name.size()] == ':';
        if (!drop) {
            if (!out.empty()) out += "\r\n";
            out += trimmed;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

std::string headersToMpv(const std::string& headers) {
    std::string out;
    out.reserve(headers.size());
    for (size_t i = 0; i < headers.size(); i++) {
        if (headers[i] == '\r') continue;
        if (headers[i] == '\n') {
            if (!out.empty() && out.back() != ',') out += ',';
            continue;
        }
        out += headers[i];
    }
    while (!out.empty() && out.back() == ',') out.pop_back();
    return out;
}

void startMpvStream(const std::string& url, const std::string& headers) {
#ifdef NXKIT_WITH_MPV
    auto& mpv = nxkit::MPVCore::instance();
    mpv.stop();
    mpv.reset();
    // Restore hwdec as well: a previous file may have fallen back to software.
    mpv.command_async("set", "hwdec", "auto");
    // ffmpeg would send mpv's own (Lavf/mpv) User-Agent instead of the one the
    // provider signed the link with, which makes some CDNs answer 403.
    std::string userAgent = headerValue(headers, "User-Agent");
    std::string extraHeaders = headersWithout(headers, "User-Agent");
    if (!userAgent.empty()) mpv.command_async("set", "user-agent", userAgent);
    mpv.setHttpHeaders(extraHeaders.empty() ? "" : headersToMpv(extraHeaders));
    mpv.setUrl(url);
#else
    (void)url;
    (void)headers;
#endif
}

}  // namespace cloud
