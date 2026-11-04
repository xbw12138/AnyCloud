//
// cloud: Quark drive provider (pan.quark.cn).
//

#include "cloud/drive/quark_drive.hpp"

#include <cstdio>
#include <exception>
#include <random>

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
    std::lock_guard<std::mutex> lock(cookieMutex);
    http.setCookie(nxkit::AuthStore::instance().get("cloud.quark.cookie"));
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
    std::lock_guard<std::mutex> lock(cookieMutex);
    http.setCookie(nxkit::mergeCookies(http.cookie(), result.cookies));
}

std::string QuarkDrive::playbackHeaders() const {
    std::lock_guard<std::mutex> lock(cookieMutex);
    // mpv's http-header-fields is a comma separated string list.
    return "Cookie: " + http.cookie() + ",Referer: " + kReferer +
           ",User-Agent: " + kUserAgent;
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
                info.nickname =
                    root.value("data", json::object()).value("nickname", "");
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

}  // namespace cloud
