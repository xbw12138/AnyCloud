//
// cloud: WebDAV client built directly on libcurl, which is the only http stack
// available here that lets us send PROPFIND / MKCOL / MOVE.
//

#include "cloud/drive/webdav_session.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include <curl/curl.h>

#include "cloud/util/crypto.hpp"

namespace cloud {

namespace {

const char* kPropfindBody =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\"><d:prop>"
    "<d:displayname/><d:resourcetype/><d:getcontentlength/>"
    "<d:getlastmodified/>"
    "</d:prop></d:propfind>";

std::string trim(const std::string& text) {
    size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

size_t writeBody(char* data, size_t size, size_t count, void* userdata) {
    auto* target = (std::string*)userdata;
    target->append(data, size * count);
    return size * count;
}

/// XML tag walking -----------------------------------------------------------
/// WebDAV servers disagree on namespace prefixes ("d:", "D:", "lp1:" or a
/// default namespace), so every lookup compares the local part only.

/// Local part of a tag name, lowercased ("D:href" -> "href").
std::string localName(const std::string& qualified) {
    size_t colon = qualified.rfind(':');
    return lower(colon == std::string::npos ? qualified
                                            : qualified.substr(colon + 1));
}

struct Tag {
    bool valid = false;
    bool selfClosing = false;
    std::string name;           // lowercased local name
    size_t openPos = 0;         // index of '<'
    size_t openEnd = 0;         // index just after '>'
    size_t closePos = std::string::npos;  // index of '<' of the end tag
};

/// Splits one "<...>" chunk into tag name and shape flags.
bool readTag(const std::string& xml, size_t lt, bool& closing, bool& selfClosing,
             std::string& name, size_t& end) {
    size_t gt = xml.find('>', lt);
    if (gt == std::string::npos) return false;
    end = gt + 1;
    std::string inside = xml.substr(lt + 1, gt - lt - 1);
    if (inside.empty()) return false;
    if (inside[0] == '?' || inside[0] == '!') return false;  // decl / comment
    closing = inside[0] == '/';
    if (closing) inside.erase(inside.begin());
    selfClosing = !inside.empty() && inside.back() == '/';
    if (selfClosing) inside.pop_back();
    size_t space = inside.find_first_of(" \t\r\n");
    std::string qualified =
        inside.substr(0, space == std::string::npos ? inside.size() : space);
    if (qualified.empty()) return false;
    name = localName(qualified);
    return true;
}

/// Finds the end tag matching `name`, starting at `from`.
size_t findEndTag(const std::string& xml, const std::string& name, size_t from) {
    size_t pos = from;
    while (pos < xml.size()) {
        size_t lt = xml.find('<', pos);
        if (lt == std::string::npos) break;
        bool closing = false;
        bool selfClosing = false;
        std::string tagName;
        size_t end = 0;
        if (!readTag(xml, lt, closing, selfClosing, tagName, end)) {
            pos = lt + 1;
            continue;
        }
        if (closing && tagName == name) return lt;
        pos = end;
    }
    return std::string::npos;
}

/// Next element (or self closing element) whose local name is `name`.
Tag findTag(const std::string& xml, const std::string& name, size_t from) {
    Tag tag;
    size_t pos = from;
    while (pos < xml.size()) {
        size_t lt = xml.find('<', pos);
        if (lt == std::string::npos) break;
        bool closing = false;
        bool selfClosing = false;
        std::string tagName;
        size_t end = 0;
        if (!readTag(xml, lt, closing, selfClosing, tagName, end)) {
            pos = lt + 1;
            continue;
        }
        if (closing || tagName != name) {
            pos = end;
            continue;
        }
        tag.valid = true;
        tag.selfClosing = selfClosing;
        tag.name = tagName;
        tag.openPos = lt;
        tag.openEnd = end;
        tag.closePos = selfClosing ? end : findEndTag(xml, name, end);
        return tag;
    }
    return tag;
}

/// Text content of the first element called `name` inside [from, until).
std::string textOf(const std::string& xml, const std::string& name, size_t from,
                   size_t until) {
    size_t limit = until == std::string::npos ? xml.size() : until;
    Tag tag = findTag(xml, name, from);
    if (!tag.valid || tag.openPos >= limit) return std::string();
    if (tag.selfClosing || tag.closePos == std::string::npos) return std::string();
    return trim(xml.substr(tag.openEnd, tag.closePos - tag.openEnd));
}

/// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
int64_t daysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + (int64_t)doe - 719468;
}

/// RFC 1123 date ("Wed, 21 Oct 2015 07:28:00 GMT") to unix seconds.
int64_t parseHttpDate(const std::string& text) {
    if (text.empty()) return 0;
    char weekday[8] = {0};
    char month[8] = {0};
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (sscanf(text.c_str(), "%7s %d %7s %d %d:%d:%d", weekday, &day, month,
               &year, &hour, &minute, &second) != 7)
        return 0;

    static const char* kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                    "jul", "aug", "sep", "oct", "nov", "dec"};
    const std::string key = lower(month);
    int index = -1;
    for (int i = 0; i < 12; i++)
        if (key.compare(0, 3, kMonths[i]) == 0) index = i;
    if (index < 0 || day < 1 || day > 31 || year < 1970) return 0;

    const int64_t days = daysFromCivil(year, (unsigned)index + 1, (unsigned)day);
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

/// "" on success, a readable reason otherwise.
std::string statusReason(long status) {
    switch (status) {
        case 301:
        case 302:
        case 307:
        case 308:
            return "地址被服务器重定向，请检查是否缺少结尾的 /";
        case 401:
            return "用户名或密码不正确";
        case 403:
            return "没有访问权限";
        case 404:
            return "路径不存在";
        case 405:
            return "服务器不允许该操作";
        case 409:
            return "父目录不存在";
        case 423:
            return "文件被服务器锁定";
        case 507:
            return "服务器空间不足";
        default:
            return "HTTP " + std::to_string(status);
    }
}

struct UploadContext {
    FILE* file = nullptr;
    std::function<void(uint64_t, uint64_t)> progress;
};

size_t readLocalFile(char* buffer, size_t size, size_t count, void* userdata) {
    auto* context = (UploadContext*)userdata;
    return fread(buffer, 1, size * count, context->file);
}

int reportUpload(void* clientp, curl_off_t, curl_off_t, curl_off_t total,
                 curl_off_t now) {
    auto* context = (UploadContext*)clientp;
    if (context->progress && total > 0)
        context->progress((uint64_t)now, (uint64_t)total);
    return 0;
}

}  // namespace

std::string webDavNormalizeUrl(const std::string& url) {
    std::string value = trim(url);
    if (value.empty()) return value;
    // A bare host ("nas.local:5005/dav") is far more likely to be a plain http
    // server; https users can type the scheme explicitly.
    if (value.find("://") == std::string::npos) value = "http://" + value;
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
}

std::string webDavNormalize(const std::string& path) {
    std::string value = trim(path);
    if (value.empty() || value == "/") return "/";
    if (!value.empty() && value[0] != '/') value.insert(value.begin(), '/');
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
}

std::string webDavJoin(const std::string& dir, const std::string& name) {
    const std::string base = webDavNormalize(dir);
    if (name.empty()) return base;
    if (base == "/") return "/" + name;
    return base + "/" + name;
}

std::string webDavParent(const std::string& path) {
    const std::string value = webDavNormalize(path);
    size_t slash = value.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return "/";
    return value.substr(0, slash);
}

std::string webDavBaseName(const std::string& path) {
    const std::string value = webDavNormalize(path);
    size_t slash = value.find_last_of('/');
    if (slash == std::string::npos) return value;
    return value.substr(slash + 1);
}

std::string webDavEncodePath(const std::string& path) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size());
    for (unsigned char c : path) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~' || c == '/') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string webDavDecodePath(const std::string& encoded) {
    std::string out;
    out.reserve(encoded.size());
    for (size_t i = 0; i < encoded.size(); i++) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(encoded[i + 1]);
            const int lo = hex(encoded[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(encoded[i]);
    }
    return out;
}

std::string webDavBasePath(const std::string& baseUrl) {
    const size_t scheme = baseUrl.find("://");
    if (scheme == std::string::npos) return "/";
    size_t slash = baseUrl.find('/', scheme + 3);
    if (slash == std::string::npos) return "/";
    return webDavNormalize(baseUrl.substr(slash));
}

std::string webDavUrlFor(const std::string& baseUrl, const std::string& path) {
    const std::string base = webDavNormalizeUrl(baseUrl);
    const std::string normalized = webDavNormalize(path);
    // A collection url keeps the trailing slash it was given: several servers
    // answer 301 otherwise, and a redirected PROPFIND loses its body.
    const bool collection = normalized != "/" && !path.empty() && path.back() == '/';
    if (normalized == "/") return base + "/";
    return base + webDavEncodePath(normalized) + (collection ? "/" : "");
}

std::string webDavAuthHeader(const WebDavConfig& config) {
    if (config.anonymous()) return std::string();
    return "Authorization: Basic " +
           crypto::base64Encode(config.user + ":" + config.password);
}

WebDavSession::WebDavSession(const WebDavConfig& config)
    : credentials(config) {}

WebDavSession::Response WebDavSession::request(
    const std::string& method, const std::string& path,
    const std::vector<std::string>& extraHeaders, const std::string& body) {
    Response response;
    CURL* curl = curl_easy_init();
    if (!curl) {
        response.error = "webdav: cannot start the http client";
        return response;
    }

    const std::string url = webDavUrlFor(this->credentials.url, path);
    struct curl_slist* headers = nullptr;
    const std::string auth = webDavAuthHeader(this->credentials);
    if (!auth.empty()) headers = curl_slist_append(headers, auth.c_str());
    for (const std::string& line : extraHeaders)
        headers = curl_slist_append(headers, line.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#ifdef __SWITCH__
    // No trust store and a usually wrong clock, like the other http paths.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
#endif
    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         (curl_off_t)body.size());
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);

    const CURLcode code = curl_easy_perform(curl);
    if (code == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    else
        response.error = std::string("webdav: ") + curl_easy_strerror(code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    this->status = response.status;
    response.ok = code == CURLE_OK;
    return response;
}

std::string WebDavSession::describe(const Response& response) const {
    if (!response.ok) return response.error.empty() ? "webdav: request failed"
                                                    : response.error;
    return "webdav: " + statusReason(response.status);
}

bool WebDavSession::probe(std::string& error) {
    Response response = this->request(
        "PROPFIND", "/", {"Depth: 0", "Content-Type: application/xml; charset=utf-8"},
        kPropfindBody);
    if (!response.ok) {
        error = describe(response);
        return false;
    }
    // 207 is the multistatus answer; a few servers reply 200 for depth 0.
    if (response.status != 207 && response.status != 200) {
        error = describe(response);
        return false;
    }
    return true;
}

bool WebDavSession::list(const std::string& dirPath,
                         std::vector<WebDavEntry>& entries, std::string& error) {
    const std::string dir = webDavNormalize(dirPath);
    Response response = this->request(
        "PROPFIND", dir == "/" ? "/" : dir + "/",
        {"Depth: 1", "Content-Type: application/xml; charset=utf-8"},
        kPropfindBody);
    if (!response.ok) {
        error = describe(response);
        return false;
    }
    if (response.status != 207 && response.status != 200) {
        error = describe(response);
        return false;
    }

    const std::string basePath = webDavBasePath(this->credentials.url);
    const std::string self = dir;

    size_t pos = 0;
    while (pos < response.body.size()) {
        Tag block = findTag(response.body, "response", pos);
        if (!block.valid) break;
        const size_t blockEnd = block.closePos == std::string::npos
                                    ? response.body.size()
                                    : block.closePos;
        const size_t start = block.openEnd;

        std::string href = textOf(response.body, "href", start, blockEnd);
        pos = blockEnd + 1;
        if (href.empty()) continue;

        // Normalise the server's href into a path inside the collection.
        if (href.compare(0, 7, "http://") == 0 ||
            href.compare(0, 8, "https://") == 0) {
            size_t scheme = href.find("://");
            size_t slash = href.find('/', scheme + 3);
            href = slash == std::string::npos ? "/" : href.substr(slash);
        }
        href = webDavDecodePath(href);
        if (basePath != "/" &&
            href.compare(0, basePath.size(), basePath) == 0 &&
            (href.size() == basePath.size() || href[basePath.size()] == '/')) {
            std::string rest = href.substr(basePath.size());
            if (rest.empty()) rest = "/";
            href = rest;
        }
        const std::string path = webDavNormalize(href);
        if (path == self || path == "/") continue;
        if (path == webDavParent(self)) continue;

        const std::string name = webDavBaseName(path);
        if (name.empty()) continue;

        WebDavEntry entry;
        entry.name = name;

        Tag type = findTag(response.body, "resourcetype", start);
        if (type.valid && type.openPos < blockEnd &&
            type.closePos != std::string::npos) {
            Tag collection = findTag(response.body, "collection", type.openEnd);
            entry.isDir = collection.valid && collection.openPos < type.closePos;
        }

        const std::string length =
            textOf(response.body, "getcontentlength", start, blockEnd);
        if (!length.empty()) entry.size = (uint64_t)strtoull(length.c_str(), nullptr, 10);
        entry.mtime = parseHttpDate(
            textOf(response.body, "getlastmodified", start, blockEnd));

        entries.push_back(std::move(entry));
    }
    return true;
}

bool WebDavSession::mkcol(const std::string& path, std::string& error) {
    // MKCOL takes no body: a server that wants one answers 415.
    Response response = this->request("MKCOL", path, {}, "");
    if (!response.ok || (response.status != 201 && response.status != 405)) {
        error = describe(response);
        return false;
    }
    if (response.status == 405) {
        error = "webdav: 同名目录已存在";
        return false;
    }
    return true;
}

bool WebDavSession::remove(const std::string& path, std::string& error) {
    Response response = this->request("DELETE", path, {}, "");
    if (!response.ok ||
        (response.status != 200 && response.status != 204)) {
        error = describe(response);
        return false;
    }
    return true;
}

bool WebDavSession::move(const std::string& from, const std::string& to,
                         std::string& error) {
    const std::string destination =
        "Destination: " + webDavUrlFor(this->credentials.url, to);
    Response response =
        this->request("MOVE", from, {destination, "Overwrite: F"}, "");
    if (!response.ok ||
        (response.status != 201 && response.status != 204)) {
        // 412 means the target name is taken (Overwrite: F was honoured).
        if (response.status == 412)
            error = "webdav: 目标已存在同名文件";
        else
            error = describe(response);
        return false;
    }
    return true;
}

bool WebDavSession::put(
    const std::string& path, const std::string& localPath,
    const std::function<void(uint64_t, uint64_t)>& progress, std::string& error) {
    FILE* local = fopen(localPath.c_str(), "rb");
    if (!local) {
        error = "webdav: cannot open the local file";
        return false;
    }
    fseek(local, 0, SEEK_END);
    const curl_off_t total = (curl_off_t)ftell(local);
    fseek(local, 0, SEEK_SET);

    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(local);
        error = "webdav: cannot start the http client";
        return false;
    }

    const std::string url = webDavUrlFor(this->credentials.url, path);
    struct curl_slist* headers = nullptr;
    const std::string auth = webDavAuthHeader(this->credentials);
    if (!auth.empty()) headers = curl_slist_append(headers, auth.c_str());
    // Let the server decide; some return 100-continue in a way ffmpeg-style
    // clients mishandle.
    headers = curl_slist_append(headers, "Expect:");

    UploadContext context;
    context.file = local;
    context.progress = progress;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, total);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, readLocalFile);
    curl_easy_setopt(curl, CURLOPT_READDATA, &context);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, reportUpload);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // No total timeout: a large file on a slow uplink must be able to finish.
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
#ifdef __SWITCH__
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
#endif

    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    if (code == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    fclose(local);

    this->status = status;
    if (code != CURLE_OK) {
        error = std::string("webdav: ") + curl_easy_strerror(code);
        return false;
    }
    if (status != 200 && status != 201 && status != 204) {
        error = "webdav: " + statusReason(status);
        return false;
    }
    if (progress) progress((uint64_t)total, (uint64_t)total);
    return true;
}

}  // namespace cloud
