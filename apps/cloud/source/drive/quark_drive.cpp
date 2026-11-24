//
// cloud: Quark drive provider (pan.quark.cn).
//

#include "cloud/drive/quark_drive.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <exception>
#include <random>
#include <vector>

#include "cloud/util/crypto.hpp"
#include "cloud/util/file_upload.hpp"
#include "cpr/async.h"
#include "nxkit/auth_store.hpp"

using json = nlohmann::json;

namespace cloud {

namespace {

// Temporary on-device diagnostics: writes to the same log the QR view uses.
void netDebugLog(const std::string& line) {
#ifdef __SWITCH__
    FILE* file = fopen("sdmc:/switch/nxkit/qr_debug.log", "a");
#else
    FILE* file = fopen("/tmp/qr_debug.log", "a");
#endif
    if (!file) return;
    fprintf(file, "%s\n", line.c_str());
    fclose(file);
}

const char* kDriveBase = "https://drive.quark.cn/1/clouddrive";
const char* kUopBase = "https://uop.quark.cn/cas/ajax";
const char* kReferer = "https://pan.quark.cn";
// No commas: mpv treats http-header-fields as a comma separated list, so a
// comma inside the value splits the header and ffmpeg sends a malformed
// request line (the CDN answers 400 Bad Request).
const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "quark-cloud-drive/3.14.2 Chrome/112.0.5615.165 "
    "Electron/24.1.3.4 Safari/537.36 Channel/pckk_other_ch";

const nxkit::Query kCommonQuery = {{"pr", "ucpro"}, {"fr", "pc"}};

std::string uuid4() {
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> hex(0, 15);
    const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            out += '-';
        } else if (i == 14) {
            out += '4';
        } else if (i == 19) {
            out += digits[8 + hex(rng) % 4];
        } else {
            out += digits[hex(rng)];
        }
    }
    return out;
}

/// Extracts the error message from a Quark api response, or empty on success.
std::string apiError(const json& root) {
    if (root.is_discarded()) return "invalid response";
    if (root.value("code", -1) == 0) return "";
    return root.value("message", "unknown error");
}

std::string resolveLocation(const std::string& base, const std::string& location) {
    if (location.compare(0, 4, "http") == 0) return location;
    // scheme://host
    size_t schemeEnd = base.find("://");
    if (schemeEnd == std::string::npos) return location;
    size_t hostEnd = base.find('/', schemeEnd + 3);
    std::string origin =
        hostEnd == std::string::npos ? base : base.substr(0, hostEnd);
    if (!location.empty() && location[0] == '/') return origin + location;
    return location;
}

void runAsync(std::function<void()> task) {
    // Use cpr's worker pool instead of a bare detached std::thread: detached
    // threads crash on the Switch, and an uncaught exception there would kill
    // the whole app.
    cpr::async([task = std::move(task)]() mutable {
        try {
            task();
        } catch (const std::exception& e) {
            netDebugLog(std::string("async: exception ") + e.what());
        }
    });
}

}  // namespace

QuarkDrive::QuarkDrive() {
    http.setBaseUrl(kDriveBase);
    http.setHeader("User-Agent", kUserAgent);
    http.setHeader("Referer", kReferer);
    http.setTimeout(15000);
}

void QuarkDrive::loadCredentials() {
    std::string cookie;
    {
        std::lock_guard<std::mutex> lock(cookieMutex);
        http.setCookie(nxkit::AuthStore::instance().get("cloud.quark.cookie"));
        cookie = http.cookie();
    }
    // The session cookie used by the thumbnail endpoints (`__puus`) expires
    // after about a day. /config hands back a fresh one, so a stored cookie
    // keeps working for covers after a restart.
    if (!cookie.empty()) refreshSessionIfStale();
}

void QuarkDrive::refreshSessionIfStale() {
    const int64_t interval = 30 * 60;  // seconds
    int64_t now = (int64_t)time(nullptr);
    {
        std::lock_guard<std::mutex> lock(cookieMutex);
        if (now - sessionRefreshedAt < interval) return;
        sessionRefreshedAt = now;
    }
    // Cookies rotated by this response are persisted by the merge in driveGet.
    driveGet("/config", {}, [](const json&, const std::string&) {});
}

bool QuarkDrive::isLoggedIn() const {
    std::lock_guard<std::mutex> lock(cookieMutex);
    return !http.cookie().empty();
}

void QuarkDrive::logout() {
    {
        std::lock_guard<std::mutex> lock(cookieMutex);
        http.setCookie("");
    }
    nxkit::AuthStore::instance().remove("cloud.quark.cookie");
    nxkit::AuthStore::instance().remove("cloud.quark.nickname");
}

void QuarkDrive::saveCookie() {
    nxkit::AuthStore::instance().set("cloud.quark.cookie", http.cookie());
}

void QuarkDrive::mergeResponseCookies(const nxkit::HttpResult& result) {
    if (result.cookies.empty()) return;
    std::string merged;
    {
        std::lock_guard<std::mutex> lock(cookieMutex);
        merged = nxkit::mergeCookies(http.cookie(), result.cookies);
        if (merged == http.cookie()) return;
        http.setCookie(merged);
    }
    // Rotated cookies (e.g. __puus on every /config) must outlive the
    // process, otherwise the next start would use an expired session and the
    // thumbnail endpoints would answer 401.
    nxkit::AuthStore::instance().set("cloud.quark.cookie", merged);
}

std::string QuarkDrive::playbackHeaders() const {
    std::lock_guard<std::mutex> lock(cookieMutex);
    // One header per line: the user agent contains commas, so a comma
    // separated list cannot be split safely. mpv's consumer converts this.
    return "Cookie: " + http.cookie() + "\r\nReferer: " + kReferer +
           "\r\nUser-Agent: " + kUserAgent;
}

// -----------------------------------------------------------------------------
// QR code login
// -----------------------------------------------------------------------------

void QuarkDrive::qrToken(
    std::function<void(const std::string& qrContent, const std::string& token,
                       const std::string& error)> callback) {
    nxkit::HttpClient client;
    client.setHeader("User-Agent", kUserAgent);
    client.setHeader("Referer", kReferer);
    client.getAsync(
        std::string(kUopBase) + "/getTokenForQrcodeLogin",
        {{"client_id", "532"}, {"v", "1.2"}, {"request_id", uuid4()}},
        [callback](const nxkit::HttpResult& result) {
            char line[400];
            snprintf(line, sizeof(line), "net: qrToken ok=%d code=%d status=%ld error=%s body=%.180s",
                     (int)result.ok, result.code, result.status, result.error.c_str(),
                     result.body.c_str());
            netDebugLog(line);
            if (!result.ok) return callback("", "", result.error);
            json root = result.json();
            std::string token = root.value("data", json::object())
                                    .value("members", json::object())
                                    .value("token", "");
            if (token.empty())
                return callback("", "", root.value("message", "no token"));
            netDebugLog("net: qrToken got token");
            std::string content =
                "https://su.quark.cn/4_eMHBJ?token=" + token +
                "&client_id=532&ssb=weblogin&uc_param_str=&uc_biz_str="
                "S%3Acustom%7COPT%3ASAREA%400%7COPT%3AIMMERSIVE%401%7COPT%"
                "3ABACK_BTN_STYLE%400";
            callback(content, token, "");
        });
}

void QuarkDrive::qrPoll(
    const std::string& token,
    std::function<void(QrStatus status, const std::string& error)> callback) {
    nxkit::HttpClient client;
    client.setHeader("User-Agent", kUserAgent);
    client.setHeader("Referer", kReferer);
    client.getAsync(
        std::string(kUopBase) + "/getServiceTicketByQrcodeToken",
        {{"client_id", "532"}, {"v", "1.2"}, {"token", token},
         {"request_id", uuid4()}},
        [this, callback](const nxkit::HttpResult& result) {
            if (!result.ok) return callback(QrStatus::Waiting, "");  // retry
            json root = result.json();
            long status = root.value("status", 0L);
            if (status == 50004001) return callback(QrStatus::Waiting, "");
            if (status == 50004002) return callback(QrStatus::Expired, "");
            if (status != 2000000)
                return callback(QrStatus::Waiting,
                                root.value("message", ""));

            std::string ticket = root.value("data", json::object())
                                     .value("members", json::object())
                                     .value("service_ticket", "");
            if (ticket.empty())
                return callback(QrStatus::Waiting, "no service ticket");

            // Exchange the ticket for cookies (follows redirects manually),
            // then fill in the remaining drive cookies.
            netDebugLog("poll: got service ticket");
            runAsync([this, ticket, callback] {
                netDebugLog("poll: exchanging ticket");
                collectCookies("https://pan.quark.cn/account/info?st=" + ticket +
                               "&lw=scan");
                netDebugLog("poll: ticket exchanged");
                driveGet("/config", {},
                         [this, callback](const json&, const std::string& error) {
                             netDebugLog(("poll: config done err=" + error).c_str());
                             saveCookie();
                             netDebugLog("poll: cookie saved");
                             callback(QrStatus::Success, "");
                             netDebugLog("poll: success callback returned");
                         });
            });
        });
}

void QuarkDrive::collectCookies(const std::string& url) {
    nxkit::HttpClient client;
    client.setHeader("User-Agent", kUserAgent);
    client.setHeader("Referer", kReferer);
    client.setFollowRedirects(false);
    client.setTimeout(15000);

    std::string current = url;
    for (int hop = 0; hop < 8; hop++) {
        {
            std::lock_guard<std::mutex> lock(cookieMutex);
            client.setCookie(http.cookie());
        }
        nxkit::HttpResult result = client.get(current);
        mergeResponseCookies(result);
        bool redirect = result.status == 301 || result.status == 302 ||
                        result.status == 303 || result.status == 307 ||
                        result.status == 308;
        auto it = result.headers.find("location");
        if (!redirect || it == result.headers.end() || it->second.empty())
            break;
        current = resolveLocation(current, it->second);
    }
    saveCookie();
}

// -----------------------------------------------------------------------------
// Account
// -----------------------------------------------------------------------------

void QuarkDrive::accountInfo(
    std::function<void(const AccountInfo& info, const std::string& error)>
        callback) {
    runAsync([this, callback] {
        AccountInfo info;

        {  // nickname (pan.quark.cn)
            nxkit::HttpClient client;
            client.setHeader("User-Agent", kUserAgent);
            client.setHeader("Referer", kReferer);
            {
                std::lock_guard<std::mutex> lock(cookieMutex);
                client.setCookie(http.cookie());
            }
            nxkit::HttpResult result = client.get(
                "https://pan.quark.cn/account/info", kCommonQuery);
            mergeResponseCookies(result);
            if (result.ok) {
                json root = result.json();
                json data = root.value("data", json::object());
                info.nickname = data.value("nickname", "");
                info.avatarUrl =
                    data.value("avatar_url", data.value("avatar", ""));
            }
        }

        std::string cookie;
        {
            std::lock_guard<std::mutex> lock(cookieMutex);
            cookie = http.cookie();
        }
        nxkit::HttpClient client;
        client.setBaseUrl(kDriveBase);
        client.setHeader("User-Agent", kUserAgent);
        client.setHeader("Referer", kReferer);
        client.setCookie(cookie);
        nxkit::HttpResult result =
            client.get("/member", {{"fetch_subscribe", "false"},
                                   {"_ch", "home"},
                                   {"fetch_identity", "false"},
                                   {"pr", "ucpro"},
                                   {"fr", "pc"}});
        mergeResponseCookies(result);
        if (!result.ok) return callback(info, result.error);
        json root = result.json();
        std::string error = apiError(root);
        if (!error.empty()) return callback(info, error);

        json data = root.value("data", json::object());
        info.memberType = data.value("member_type", "");
        info.usedCapacity = data.value("use_capacity", (uint64_t)0);
        info.totalCapacity = data.value("total_capacity", (uint64_t)0);
        if (!info.nickname.empty())
            nxkit::AuthStore::instance().set("cloud.quark.nickname",
                                             info.nickname);
        callback(info, "");
    });
}

// -----------------------------------------------------------------------------
// Files
// -----------------------------------------------------------------------------

void QuarkDrive::driveGet(const std::string& path, const nxkit::Query& query,
                          JsonCallback callback) {
    nxkit::Query full = kCommonQuery;
    full.insert(full.end(), query.begin(), query.end());
    nxkit::HttpClient client;
    {
        std::lock_guard<std::mutex> lock(cookieMutex);
        client = http;  // copies base url, headers and cookie
    }
    client.getAsync(path, full,
                    [this, callback](const nxkit::HttpResult& result) {
                        if (!result.ok) return callback(json{}, result.error);
                        json root = result.json();
                        mergeResponseCookies(result);
                        std::string error = apiError(root);
                        if (!error.empty()) return callback(json{}, error);
                        callback(root, "");
                    });
}

void QuarkDrive::drivePost(const std::string& path, const json& body,
                           JsonCallback callback) {
    nxkit::HttpClient client;
    {
        std::lock_guard<std::mutex> lock(cookieMutex);
        client = http;
    }
    client.postJsonAsync(path, kCommonQuery, body,
                         [this, callback](const nxkit::HttpResult& result) {
                             if (!result.ok)
                                 return callback(json{}, result.error);
                             json root = result.json();
                             mergeResponseCookies(result);
                             std::string error = apiError(root);
                             if (!error.empty()) return callback(json{}, error);
                             callback(root, "");
                         });
}

void QuarkDrive::listFiles(
    const std::string& dirId, int page, int pageSize,
    std::function<void(const std::vector<DriveFile>& files, int total,
                       const std::string& error)> callback) {
    refreshSessionIfStale();
    driveGet("/file/sort",
             {{"pdir_fid", dirId},
              {"_page", std::to_string(page)},
              {"_size", std::to_string(pageSize)},
              {"_fetch_total", "1"},
              {"fetch_all_file", "1"},
              {"fetch_risk_file_name", "1"},
              {"_sort", "file_type:asc,updated_at:desc"}},
             [callback](const json& root, const std::string& error) {
                 if (!error.empty())
                     return callback(std::vector<DriveFile>{}, -1, error);
                 std::vector<DriveFile> files;
                 json list = root.value("data", json::object())
                                 .value("list", json::array());
                 for (const auto& item : list) {
                     DriveFile file;
                     file.id = item.value("fid", "");
                     file.name = item.value("file_name", "");
                     file.isDir = item.value("dir", false);
                     file.size = item.value("size", (uint64_t)0);
                     file.updatedAt = item.value("updated_at", (int64_t)0);
                     file.category = item.value("category", 0);
                     // Cover image URL used by the grid/list cards. Quark
                     // returns `thumbnail` for videos and images (larger
                     // variants use `big_thumbnail`).
                     file.thumb = item.value("thumbnail", "");
                     if (file.thumb.empty())
                         file.thumb = item.value("big_thumbnail", "");
                     if (file.thumb.empty())
                         file.thumb = item.value("thumb_url", "");
                     // Fallback for payloads without a poster url: videos and
                     // images have separate thumbnail endpoints (the generic
                     // one rejects video fids with 400).
                     if (file.thumb.empty() && !file.isDir) {
                         std::string objectCategory =
                             item.value("obj_category", "");
                         if (objectCategory == "video" || file.category == 1)
                             file.thumb =
                                 "https://drive-pc.quark.cn/1/clouddrive/file/"
                                 "video/thumbnail?fid=" +
                                 file.id + "&pr=ucpro&fr=pc";
                         else if (objectCategory == "image" ||
                                  file.category == 3)
                             file.thumb =
                                 "https://drive-pc.quark.cn/1/clouddrive/file/"
                                 "thumbnail?fid=" +
                                 file.id + "&size=400&pr=ucpro&fr=pc";
                     }
                     files.push_back(std::move(file));
                 }
                 int total = root.value("metadata", json::object())
                                 .value("_total", -1);
                 callback(files, total, "");
             });
}

void QuarkDrive::downloadInfo(
    const std::string& fileId,
    std::function<void(const std::string& url, const std::string& headers,
                       const std::string& error)> callback) {
    drivePost("/file/download", json{{"fids", {fileId}}},
              [this, callback](const json& root, const std::string& error) {
                  if (!error.empty()) return callback("", "", error);
                  json data = root.value("data", json::array());
                  if (data.empty())
                      return callback("", "", "no download url");
                  std::string url = data[0].value("download_url", "");
                  if (url.empty()) return callback("", "", "no download url");
                  callback(url, playbackHeaders(), "");
              });
}

void QuarkDrive::videoStreams(
    const std::string& fileId,
    std::function<void(const std::vector<VideoStream>& streams,
                       const std::string& defaultResolution,
                       const std::string& error)> callback) {
    json body = {{"fid", fileId},
                 {"resolutions", "normal,high,super,2k,4k"},
                 {"supports", "fmp4_av,m3u8,dolby_vision"}};
    // Note: "/file/v2/play", not "/file/v2/play/project" (the latter rejects
    // this body with 14018 plf_invalid). While the server is still transcoding
    // it answers 21002, which callers treat as "fall back to the original file".
    drivePost("/file/v2/play", body,
              [callback](const json& root, const std::string& error) {
                  if (!error.empty())
                      return callback(std::vector<VideoStream>{}, "", error);
                  std::vector<VideoStream> streams;
                  json data = root.value("data", json::object());
                  json list = data.value("video_list", json::array());
                  for (const auto& item : list) {
                      if (item.value("trans_status", std::string()) != "success")
                          continue;
                      if (!item.value("accessable", true)) continue;
                      const json vi = item.value("video_info", json::object());
                      VideoStream stream;
                      stream.resolution = item.value("resolution", "");
                      stream.url = vi.value("url", "");
                      stream.width = vi.value("width", 0);
                      stream.height = vi.value("height", 0);
                      stream.bitrate = vi.value("bitrate", (int64_t)0);
                      stream.duration = vi.value("duration", (int64_t)0);
                      if (!stream.url.empty())
                          streams.push_back(std::move(stream));
                  }
                  std::string def = data.value("default_resolution", "");
                  callback(streams, def, "");
              });
}

// -----------------------------------------------------------------------------
// File operations
// -----------------------------------------------------------------------------

void QuarkDrive::makeDir(const std::string& parentId, const std::string& name,
                         SimpleCallback callback) {
    json body = {{"pdir_fid", parentId},
                 {"file_name", name},
                 {"dir_path", ""},
                 {"dir_init_lock", false}};
    drivePost("/file", body, [callback](const json&, const std::string& error) {
        callback(error);
    });
}

void QuarkDrive::rename(const std::string& fileId, const std::string& newName,
                        SimpleCallback callback) {
    json body = {{"fid", fileId}, {"file_name", newName}};
    drivePost("/file/rename", body,
              [callback](const json&, const std::string& error) {
                  callback(error);
              });
}

void QuarkDrive::remove(const std::vector<std::string>& fileIds,
                        SimpleCallback callback) {
    json body = {{"filelist", fileIds},
                 {"action_type", 1},
                 {"exclude_fids", json::array()}};
    drivePost("/file/delete", body,
              [callback](const json&, const std::string& error) {
                  callback(error);
              });
}

void QuarkDrive::move(const std::vector<std::string>& fileIds,
                      const std::string& targetDirId, SimpleCallback callback) {
    json body = {{"filelist", fileIds},
                 {"to_pdir_fid", targetDirId},
                 {"action_type", 1},
                 {"exclude_fids", json::array()}};
    drivePost("/file/move", body,
              [callback](const json&, const std::string& error) {
                  callback(error);
              });
}

// -----------------------------------------------------------------------------
// Upload (pre -> update/hash -> per-part OSS auth + PUT -> complete -> finish)
// -----------------------------------------------------------------------------

namespace {

const char* kOssUserAgent =
    "aliyun-sdk-js/6.6.1 Chrome 98.0.4758.80 on Windows 10 64-bit";

std::string mimeTypeFor(const std::string& name) {
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = name.substr(dot + 1);
    for (char& c : ext)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (ext == "mp4" || ext == "m4v") return "video/mp4";
    if (ext == "mkv") return "video/x-matroska";
    if (ext == "avi") return "video/x-msvideo";
    if (ext == "mov") return "video/quicktime";
    if (ext == "webm") return "video/webm";
    if (ext == "ts") return "video/mp2t";
    if (ext == "mp3") return "audio/mpeg";
    if (ext == "flac") return "audio/flac";
    if (ext == "m4a") return "audio/mp4";
    if (ext == "wav") return "audio/wav";
    if (ext == "srt" || ext == "ass" || ext == "ssa") return "text/plain";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "png") return "image/png";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "pdf") return "application/pdf";
    if (ext == "epub") return "application/epub+zip";
    if (ext == "txt") return "text/plain";
    if (ext == "zip") return "application/zip";
    return "application/octet-stream";
}

/// "Wed, 21 Oct 2015 07:28:00 GMT"
std::string httpDateNow() {
    time_t now = time(nullptr);
    struct tm tmv {};
    gmtime_r(&now, &tmv);
    char buffer[64];
    strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &tmv);
    return buffer;
}

/// base64 of the raw MD5 digest (OSS Content-MD5 format).
std::string md5Base64(const std::string& data) {
    std::string hex = crypto::md5Hex(data);
    std::string raw;
    raw.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto nibble = [](char c) -> int {
            return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
        };
        raw += (char)((nibble(hex[i]) << 4) | nibble(hex[i + 1]));
    }
    return crypto::base64Encode(raw);
}

std::string stripTrailingSlashes(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

std::string snippet(const std::string& body, size_t limit = 200) {
    return body.size() <= limit ? body : body.substr(0, limit) + "...";
}

}  // namespace

void QuarkDrive::uploadFile(const std::string& parentId, const std::string& name,
                            const std::string& localPath, UploadCallback progress,
                            SimpleCallback callback) {
    const uint64_t total = localFileSize(localPath);
    FILE* probe = fopen(localPath.c_str(), "rb");
    if (!probe) return callback("cloud/upload/file_missing");
    fclose(probe);

    const std::string mime = mimeTypeFor(name);
    cpr::async([this, parentId, name, localPath, total, mime, progress,
                callback] {
        const std::string base = "https://drive-pc.quark.cn/1/clouddrive";

        nxkit::HttpClient client;
        {
            std::lock_guard<std::mutex> lock(cookieMutex);
            client = http;
        }

        // Synchronous helper: keeps the cookie jar in sync on every reply.
        auto post = [this, &client](const std::string& path, const json& body,
                                    json& root) -> std::string {
            nxkit::HttpResult result =
                client.postJson(path, kCommonQuery, body);
            if (!result.cookies.empty()) {
                std::string merged;
                {
                    std::lock_guard<std::mutex> lock(cookieMutex);
                    http.setCookie(
                        nxkit::mergeCookies(http.cookie(), result.cookies));
                    merged = http.cookie();
                }
                client.setCookie(merged);
                saveCookie();
            }
            if (!result.ok) {
                netDebugLog("quark: upload post " + path + " status=" +
                            std::to_string(result.status) +
                            " body=" + snippet(result.body));
                return result.error.empty()
                           ? "http " + std::to_string(result.status)
                           : result.error;
            }
            root = result.json();
            if (root.is_discarded()) return "invalid response";
            if (root.value("code", -1) != 0)
                return root.value("message", std::string("upload failed"));
            return "";
        };

        const int64_t nowMs = (int64_t)time(nullptr) * 1000;

        // 1. Pre-upload: reserves the object and returns the OSS location.
        json pre;
        std::string error = post(base + "/file/upload/pre",
                                 {{"ccp_hash_update", true},
                                  {"dir_name", ""},
                                  {"file_name", name},
                                  {"format_type", mime},
                                  {"l_created_at", nowMs},
                                  {"l_updated_at", nowMs},
                                  {"pdir_fid", parentId},
                                  {"size", total}},
                                 pre);
        if (!error.empty()) {
            netDebugLog("quark: pre failed " + error);
            return callback("cloud/upload/failed");
        }
        json data = pre.value("data", json::object());
        const std::string taskId = data.value("task_id", "");
        const std::string uploadId = data.value("upload_id", "");
        const std::string objKey = data.value("obj_key", "");
        const std::string bucket = data.value("bucket", "");
        const std::string uploadUrl = data.value("upload_url", "");
        json authInfo = data.contains("auth_info") ? data["auth_info"] : json();
        json callbackInfo =
            data.contains("callback") ? data["callback"] : json();
        uint64_t partSize = pre.value("metadata", json::object())
                                 .value("part_size", (uint64_t)0);
        if (partSize == 0) partSize = 4ULL * 1024 * 1024;
        if (uploadId.empty() || objKey.empty() || bucket.empty() ||
            uploadUrl.empty()) {
            netDebugLog("quark: pre missing fields body=" + snippet(pre.dump()));
            return callback("cloud/upload/failed");
        }
        netDebugLog("quark: pre ok bucket=" + bucket + " host=" + uploadUrl +
                    " part=" + std::to_string(partSize));

        // 2. Whole-file hash: the server may already know this content.
        std::string md5 = crypto::md5FileHex(localPath, 0, total);
        std::string sha1 = crypto::sha1FileHex(localPath);
        if (md5.empty() || sha1.empty())
            return callback("cloud/upload/read_failed");
        netDebugLog("quark: hash computed md5=" + md5);
        json hashRoot;
        error = post(base + "/file/update/hash",
                     {{"md5", md5}, {"sha1", sha1}, {"task_id", taskId}},
                     hashRoot);
        if (!error.empty()) {
            netDebugLog("quark: hash failed " + error);
            return callback("cloud/upload/failed");
        }
        netDebugLog("quark: hash accepted");
        if (hashRoot.value("data", json::object()).value("finish", false)) {
            if (progress) progress(total, total);
            return callback("");
        }

        // The OSS host is the bucket sub-domain of the pre-upload URL.
        std::string host = uploadUrl;
        if (host.compare(0, 8, "https://") == 0)
            host = host.substr(8);
        else if (host.compare(0, 7, "http://") == 0)
            host = host.substr(7);
        host = stripTrailingSlashes(host);
        const std::string ossPath = bucket + "." + host + "/" + objKey;
        const std::string ossBase = "https://" + ossPath;
        netDebugLog("quark: oss host=" + bucket + "." + host);

        // 3. Upload every part: each one needs its own OSS signature.
        std::vector<std::string> etags;
        const uint64_t parts = total == 0 ? 1 : (total + partSize - 1) / partSize;
        for (uint64_t i = 0; i < parts; i++) {
            const uint64_t number = i + 1;
            const std::string date = httpDateNow();
            const std::string authMeta =
                "PUT\n\n" + mime + "\n" + date + "\nx-oss-date:" + date +
                "\nx-oss-user-agent:" + kOssUserAgent + "\n/" + bucket + "/" +
                objKey + "?partNumber=" + std::to_string(number) +
                "&uploadId=" + uploadId;
            json authRoot;
            error = post(base + "/file/upload/auth",
                         {{"auth_info", authInfo},
                          {"auth_meta", authMeta},
                          {"task_id", taskId}},
                         authRoot);
            if (!error.empty()) {
                netDebugLog("quark: auth part " + std::to_string(number) +
                            " failed " + error);
                return callback("cloud/upload/failed");
            }
            const std::string authKey =
                authRoot.value("data", json::object()).value("auth_key", "");
            if (authKey.empty()) {
                netDebugLog("quark: auth part " + std::to_string(number) +
                            " empty body=" + snippet(authRoot.dump()));
                return callback("cloud/upload/failed");
            }

            const std::string headers =
                "Authorization: " + authKey + "\r\nContent-Type: " + mime +
                "\r\nReferer: https://pan.quark.cn/\r\nx-oss-date: " + date +
                "\r\nx-oss-user-agent: " + kOssUserAgent + "\r\n";
            const uint64_t partOffset = i * partSize;
            const uint64_t partBytes =
                std::min(partSize, total > partOffset ? total - partOffset : 0);
            UploadResponse response;
            bool ok = false;
            // Parts are idempotent: retry over https, then once over plain
            // http (the console's mbedtls stack stalls on these hosts after
            // the interim 100 response).
            static const char* kSchemes[] = {"https", "https", "http"};
            for (int attempt = 0; attempt < 3 && !ok; attempt++) {
                const std::string attemptUrl =
                    std::string(kSchemes[attempt]) + "://" + ossPath +
                    "?partNumber=" + std::to_string(number) +
                    "&uploadId=" + uploadId;
                netDebugLog("quark: put begin part " + std::to_string(number) +
                            " try=" + std::to_string(attempt + 1) + " bytes=" +
                            std::to_string(partBytes));
                ok = uploadFileRaw(
                    attemptUrl, "PUT", headers, localPath,
                    [progress, partOffset, total](uint64_t sent, uint64_t) {
                        if (progress) progress(partOffset + sent, total);
                    },
                    &response, partOffset, partSize);
                if (!ok)
                    netDebugLog("quark: put failed part " + std::to_string(number) +
                                " try=" + std::to_string(attempt + 1) +
                                " status=" + std::to_string(response.status) +
                                " error=" + response.error +
                                " body=" + snippet(response.body));
            }
            if (!ok) {
                netDebugLog("quark: part " + std::to_string(number) +
                            " gave up status=" +
                            std::to_string(response.status) + " error=" +
                            response.error);
                return callback("cloud/upload/failed");
            }
            netDebugLog("quark: put ok part " + std::to_string(number));
            // A missing ETag makes CompleteMultipartUpload reject the commit.
            auto etag = response.headers.find("etag");
            if (etag == response.headers.end()) {
                netDebugLog("quark: part " + std::to_string(number) +
                            " has no ETag");
            }
            etags.push_back(etag == response.headers.end() ? std::string()
                                                           : etag->second);
        }

        // 4. CompleteMultipartUpload: an XML body signed like the parts.
        std::string xml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<CompleteMultipartUpload>\n";
        for (size_t i = 0; i < etags.size(); i++)
            xml += "<Part><PartNumber>" + std::to_string(i + 1) +
                   "</PartNumber><ETag>" + etags[i] + "</ETag></Part>";
        xml += "\n</CompleteMultipartUpload>";

        const std::string contentMd5 = md5Base64(xml);
        const std::string callbackB64 =
            crypto::base64Encode(callbackInfo.dump());
        const std::string date = httpDateNow();
        const std::string commitMeta =
            "POST\n" + contentMd5 + "\napplication/xml\n" + date +
            "\nx-oss-callback:" + callbackB64 + "\nx-oss-date:" + date +
            "\nx-oss-user-agent:" + kOssUserAgent + "\n/" + bucket + "/" +
            objKey + "?uploadId=" + uploadId;
        json commitAuth;
        error = post(base + "/file/upload/auth",
                     {{"auth_info", authInfo},
                      {"auth_meta", commitMeta},
                      {"task_id", taskId}},
                     commitAuth);
        if (!error.empty()) {
            netDebugLog("quark: commit auth failed " + error);
            return callback("cloud/upload/failed");
        }
        const std::string commitKey =
            commitAuth.value("data", json::object()).value("auth_key", "");
        if (commitKey.empty()) {
            netDebugLog("quark: commit auth empty body=" +
                        snippet(commitAuth.dump()));
            return callback("cloud/upload/failed");
        }

        const std::string commitHeaders =
            "Authorization: " + commitKey + "\r\nContent-MD5: " + contentMd5 +
            "\r\nContent-Type: application/xml\r\nReferer: "
            "https://pan.quark.cn/\r\nx-oss-callback: " +
            callbackB64 + "\r\nx-oss-date: " + date +
            "\r\nx-oss-user-agent: " + kOssUserAgent + "\r\n";
        UploadResponse commitResponse;
        if (!uploadRawBody(ossBase + "?uploadId=" + uploadId, "POST",
                           commitHeaders, xml, &commitResponse)) {
            netDebugLog("quark: commit status=" +
                        std::to_string(commitResponse.status) + " body=" +
                        snippet(commitResponse.body));
            return callback("cloud/upload/failed");
        }
        netDebugLog("quark: commit ok status=" +
                    std::to_string(commitResponse.status) + " body=" +
                    snippet(commitResponse.body));

        // 5. Let the drive register the finished object.
        json finish;
        error = post(base + "/file/upload/finish",
                     {{"obj_key", objKey}, {"task_id", taskId}}, finish);
        if (!error.empty()) {
            netDebugLog("quark: finish failed " + error + " body=" +
                        snippet(finish.dump()));
            return callback("cloud/upload/failed");
        }

        if (progress) progress(total, total);
        callback("");
    });
}

}  // namespace cloud
