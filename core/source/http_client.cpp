#include "nxkit/http_client.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <map>

#include "nxkit/http_log.hpp"

namespace nxkit {

namespace {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Readable request line for the in-app log; query values are kept verbatim
/// because most of them (file ids, sizes) are the interesting part.
std::string displayUrl(const std::string& url, const Query& query) {
    if (query.empty()) return url;
    std::string out = url;
    out += out.find('?') == std::string::npos ? '?' : '&';
    bool first = true;
    for (const auto& pair : query) {
        if (!first) out += '&';
        first = false;
        out += pair.first + "=" + pair.second;
    }
    return out;
}

void logRequest(const char* method, const std::string& url, int64_t startedMs,
                const HttpResult& result) {
    HttpLogEntry entry;
    entry.time = (int64_t)time(nullptr);
    entry.method = method;
    entry.url = url;
    entry.status = result.status;
    entry.code = result.code;
    entry.durationMs = (int)(nowMs() - startedMs);
    entry.bytes = result.body.size();
    entry.ok = result.ok;
    entry.error = result.error;

    auto contentType = result.headers.find("content-type");
    if (contentType != result.headers.end()) entry.note = contentType->second;
    const std::string& type = entry.note;
    const bool text = type.find("json") != std::string::npos ||
                      type.find("text") != std::string::npos ||
                      type.find("xml") != std::string::npos;
    // Text bodies explain failures ("auth expired", error codes); image bytes
    // would only pollute the log. Keep the whole body: the detail page renders
    // it in full, and HttpLog caps the total memory it retains.
    if (text && !result.body.empty()) entry.snippet = result.body;
    HttpLog::instance().add(std::move(entry));
}

}  // namespace

namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

HttpResult fromResponse(const cpr::Response& response) {
    HttpResult result;
    result.status = response.status_code;
    result.body = response.text;

    for (const auto& header : response.header) {
        std::string name = lower(header.first);
        result.headers[name] = header.second;
        if (name == "set-cookie") {
            if (!result.setCookie.empty()) result.setCookie += "; ";
            result.setCookie += header.second;
        }
    }
    for (const auto& cookie : response.cookies) {
        result.cookies[cookie.GetName()] = cookie.GetValue();
    }

    if (response.error) {
        result.code  = static_cast<int>(response.error.code);
        result.error = response.error.message;
        if (result.error.empty())
            result.error = "curl error " + std::to_string(result.code);
        return result;
    }

    result.ok = response.status_code >= 200 && response.status_code < 300;
    if (!result.ok) {
        result.error = "HTTP " + std::to_string(response.status_code);
    }
    return result;
}

}  // namespace

std::string mergeCookies(const std::string& existing,
                         const std::map<std::string, std::string>& pairs) {
    std::map<std::string, std::string> merged;
    size_t pos = 0;
    while (pos < existing.size()) {
        size_t end = existing.find("; ", pos);
        if (end == std::string::npos) end = existing.size();
        std::string pair = existing.substr(pos, end - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && eq > 0) merged[pair.substr(0, eq)] = pair.substr(eq + 1);
        pos = end + 2;
    }
    for (const auto& pair : pairs) merged[pair.first] = pair.second;

    std::string out;
    for (const auto& pair : merged) {
        if (!out.empty()) out += "; ";
        out += pair.first + "=" + pair.second;
    }
    return out;
}

nlohmann::json HttpResult::json() const {
    nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    return parsed;
}

namespace {

std::string& defaultCaBundleStorage() {
#ifdef __SWITCH__
    // No system trust store on Switch; use the bundle baked into romfs.
    static std::string path = "romfs:/cacert.pem";
#else
    static std::string path;
#endif
    return path;
}

}  // namespace

const std::string& HttpClient::sharedCaBundle() { return defaultCaBundleStorage(); }

void HttpClient::setDefaultCaBundle(const std::string& path) { defaultCaBundleStorage() = path; }

cpr::SslOptions HttpClient::sslOptions() const {
    cpr::SslOptions options;
    options.verify_host = verifySsl;
    options.verify_peer = verifySsl;
    // Only hand libcurl a CA file when we actually verify, otherwise an
    // unreadable romfs path would make the request fail for no reason.
    if (verifySsl) options.ca_info = caBundlePath();
    return options;
}

void HttpClient::applyCaBundle(cpr::Session& session) {
    std::string path = sharedCaBundle();
    if (path.empty()) return;
    cpr::SslOptions options;
    options.ca_info = path;
    session.SetOption(options);
}

void HttpClient::setBaseUrl(const std::string& url) {
    base = url;
    while (!base.empty() && base.back() == '/') base.pop_back();
}

void HttpClient::setHeader(const std::string& name, const std::string& value) {
    extraHeaders[name] = value;
}

void HttpClient::removeHeader(const std::string& name) { extraHeaders.erase(name); }

void HttpClient::clearHeaders() { extraHeaders.clear(); }

void HttpClient::setHeaderBlock(const std::string& block) {
    // "Cookie: a=b\r\nReferer: ..." (most providers) or
    // "Cookie: a=b,Referer: ..." (mpv's http-header-fields, Quark).
    const bool perLine = block.find('\n') != std::string::npos;
    size_t pos = 0;
    while (pos < block.size()) {
        size_t end = block.find(perLine ? '\n' : ',', pos);
        if (end == std::string::npos) end = block.size();
        std::string line = block.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
            name.pop_back();
        size_t start = line.find_first_not_of(" \t", colon + 1);
        std::string value;
        if (start != std::string::npos) {
            value = line.substr(start);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
                value.pop_back();
        }
        if (!name.empty() && !value.empty()) extraHeaders[name] = value;
    }
}

std::string HttpClient::urlFor(const std::string& path) const {
    if (path.empty()) return base;
    if (path.compare(0, 7, "http://") == 0 || path.compare(0, 8, "https://") == 0) return path;
    if (path[0] == '/') return base + path;
    return base + "/" + path;
}

cpr::Header HttpClient::headers() const {
    cpr::Header header;
    for (const auto& entry : extraHeaders) header[entry.first] = entry.second;
    if (!cookieValue.empty()) header["Cookie"] = cookieValue;
    if (!bearer.empty()) header["Authorization"] = "Bearer " + bearer;
    return header;
}

int HttpClient::timeoutFor(int timeout) const { return timeout > 0 ? timeout : timeoutMs; }

cpr::Redirect HttpClient::redirect() const { return cpr::Redirect{followRedirects}; }

cpr::Parameters HttpClient::toParameters(const Query& query) {
    cpr::Parameters parameters;
    for (const auto& pair : query) parameters.Add(cpr::Parameter(pair.first, pair.second));
    return parameters;
}

cpr::Payload HttpClient::toPayload(const Query& form) {
    std::vector<cpr::Pair> pairs;
    pairs.reserve(form.size());
    for (const auto& pair : form) pairs.emplace_back(pair.first, pair.second);
    return cpr::Payload(pairs.begin(), pairs.end());
}

HttpResult HttpClient::get(const std::string& path, const Query& query, int timeout) const {
    const int64_t started = nowMs();
    cpr::Response response =
        cpr::Get(cpr::Url{urlFor(path)}, toParameters(query), headers(), redirect(), sslOptions(),
                 cpr::Timeout{timeoutFor(timeout)});
    HttpResult result = fromResponse(response);
    logRequest("GET", displayUrl(urlFor(path), query), started, result);
    return result;
}

HttpResult HttpClient::post(const std::string& path, const Query& query, const Query& form,
                            int timeout) const {
    const int64_t started = nowMs();
    cpr::Response response =
        cpr::Post(cpr::Url{urlFor(path)}, toParameters(query), headers(), toPayload(form),
                  redirect(), sslOptions(), cpr::Timeout{timeoutFor(timeout)});
    HttpResult result = fromResponse(response);
    logRequest("POST", displayUrl(urlFor(path), query), started, result);
    return result;
}

HttpResult HttpClient::postJson(const std::string& path, const Query& query,
                                const nlohmann::json& body, int timeout) const {
    cpr::Header header = headers();
    header["Content-Type"] = "application/json";

    const int64_t started = nowMs();
    cpr::Response response =
        cpr::Post(cpr::Url{urlFor(path)}, toParameters(query), header, cpr::Body{body.dump()},
                  redirect(), sslOptions(), cpr::Timeout{timeoutFor(timeout)});
    HttpResult result = fromResponse(response);
    logRequest("POST", displayUrl(urlFor(path), query), started, result);
    return result;
}

void HttpClient::getAsync(const std::string& path, const Query& query,
                          std::function<void(const HttpResult&)> callback, int timeout) const {
    const int64_t started = nowMs();
    const std::string url = displayUrl(urlFor(path), query);
    cpr::GetCallback(
        [callback, url, started](const cpr::Response& response) {
            HttpResult result = fromResponse(response);
            logRequest("GET", url, started, result);
            callback(result);
        },
        cpr::Url{urlFor(path)}, toParameters(query), headers(), redirect(), sslOptions(),
        cpr::Timeout{timeoutFor(timeout)});
}

void HttpClient::postAsync(const std::string& path, const Query& query, const Query& form,
                           std::function<void(const HttpResult&)> callback, int timeout) const {
    const int64_t started = nowMs();
    const std::string url = displayUrl(urlFor(path), query);
    cpr::PostCallback(
        [callback, url, started](const cpr::Response& response) {
            HttpResult result = fromResponse(response);
            logRequest("POST", url, started, result);
            callback(result);
        },
        cpr::Url{urlFor(path)}, toParameters(query), headers(), toPayload(form), redirect(),
        sslOptions(), cpr::Timeout{timeoutFor(timeout)});
}

void HttpClient::postJsonAsync(const std::string& path, const Query& query,
                               const nlohmann::json& body,
                               std::function<void(const HttpResult&)> callback, int timeout) const {
    cpr::Header header = headers();
    header["Content-Type"] = "application/json";

    const int64_t started = nowMs();
    const std::string url = displayUrl(urlFor(path), query);
    cpr::PostCallback(
        [callback, url, started](const cpr::Response& response) {
            HttpResult result = fromResponse(response);
            logRequest("POST", url, started, result);
            callback(result);
        },
        cpr::Url{urlFor(path)}, toParameters(query), header, cpr::Body{body.dump()}, redirect(),
        sslOptions(), cpr::Timeout{timeoutFor(timeout)});
}

}  // namespace nxkit
