//
// cloud: Aliyun Drive provider (www.aliyundrive.com, web endpoints).
//

#include "cloud/drive/aliyun_drive.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "cloud/util/crypto.hpp"
#include "cloud/util/file_upload.hpp"
#include "cloud/util/text_encoding.hpp"
#include "cpr/async.h"
#include "nxkit/auth_store.hpp"

using json = nlohmann::json;

namespace cloud {

namespace {

const char* kPassportBase = "https://passport.aliyundrive.com";
const char* kAuthBase = "https://auth.aliyundrive.com";
const char* kApiBase = "https://api.aliyundrive.com";
const char* kReferer = "https://www.aliyundrive.com/";
const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

// Fields the web login page sends with both QR calls.
const nxkit::Query kQrFields = {
    {"appName", "aliyun_drive"}, {"fromSite", "52"},
    {"appEntrance", "web"},      {"isMobile", "false"},
    {"lang", "zh_CN"},           {"returnUrl", ""},
    {"bizParams", ""},           {"_bx-v", "2.5.3"},
};

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

std::string snippet(const std::string& body, size_t limit = 200) {
    return body.size() <= limit ? body : body.substr(0, limit) + "...";
}

void runAsync(std::function<void()> task) {    cpr::async([task = std::move(task)]() mutable {
        try {
            task();
        } catch (const std::exception& e) {
            netDebugLog(std::string("aliyun: exception ") + e.what());
        }
    });
}

int64_t nowMs() { return (int64_t)time(nullptr) * 1000; }

/// Days since 1970-01-01 for a proleptic Gregorian date.
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/// "2024-03-05T12:34:56.789Z" -> milliseconds since epoch (0 on failure).
int64_t parseIsoTime(const std::string& value) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    double sec = 0;
    if (sscanf(value.c_str(), "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi,
               &sec) != 6)
        return 0;
    return (daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 +
            mi * 60 + (int64_t)sec) *
           1000;
}

uint64_t jsonSize(const json& value) {
    if (value.is_number_unsigned()) return value.get<uint64_t>();
    if (value.is_number_integer()) return (uint64_t)value.get<int64_t>();
    if (value.is_string()) {
        try {
            return (uint64_t)std::stoull(value.get<std::string>());
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

std::string base64Decode(const std::string& input) {
    static const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int table[256];
    for (int& entry : table) entry = -1;
    for (size_t i = 0; i < alphabet.size(); i++)
        table[(unsigned char)alphabet[i]] = (int)i;

    std::string out;
    out.reserve(input.size() * 3 / 4);
    int buffer = 0, bits = 0;
    for (unsigned char c : input) {
        if (c == '=') break;
        // Accept the url-safe alphabet too: dropping those bytes instead of
        // translating them silently corrupts the payload.
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        int value = table[c];
        if (value < 0) continue;  // newlines and other noise
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((buffer >> bits) & 0xFF);
        }
    }
    return out;
}

/// Extracts `"key":"value"` from raw text without a JSON parser.
std::string jsonField(const std::string& body, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    size_t pos = body.find(needle);
    if (pos == std::string::npos) {
        const std::string spaced = "\"" + key + "\": \"";
        pos = body.find(spaced);
        if (pos == std::string::npos) return "";
        pos += spaced.size();
    } else {
        pos += needle.size();
    }
    size_t end = body.find('"', pos);
    if (end == std::string::npos) return "";
    return body.substr(pos, end - pos);
}

/// aliyundrive reports errors as {"code": "...", "message": "..."}.
std::string apiError(const json& root) {
    if (root.is_discarded()) return "invalid response";
    if (!root.contains("code") || root["code"].is_null()) return "";
    std::string message = root.value("message", "");
    std::string code = root.value("code", "");
    if (message.empty()) return code.empty() ? "unknown error" : code;
    return message;
}

}  // namespace

AliyunDrive::AliyunDrive() = default;

// -----------------------------------------------------------------------------
// Credentials
// -----------------------------------------------------------------------------

void AliyunDrive::loadCredentials() {
    auto& store = nxkit::AuthStore::instance();
    std::lock_guard<std::mutex> lock(mutex);
    refreshToken = store.get("cloud.aliyun.refresh_token");
    accessToken = store.get("cloud.aliyun.access_token");
    driveId = store.get("cloud.aliyun.drive_id");
    nickname = store.get("cloud.aliyun.nickname");
    std::string expires = store.get("cloud.aliyun.expires_at");
    expiresAt = expires.empty() ? 0 : std::stoll(expires);
}

bool AliyunDrive::isLoggedIn() const {
    std::lock_guard<std::mutex> lock(mutex);
    return !refreshToken.empty();
}

void AliyunDrive::logout() { clearSession(); }

void AliyunDrive::saveSession(const std::string& access,
                              const std::string& refresh, int64_t expires,
                              const std::string& drive) {
    std::string savedAccess;
    std::string savedRefresh;
    std::string savedDrive;
    int64_t savedExpires = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!access.empty()) accessToken = access;
        if (!refresh.empty()) refreshToken = refresh;
        if (expires > 0) expiresAt = expires;
        if (!drive.empty()) driveId = drive;
        savedAccess = accessToken;
        savedRefresh = refreshToken;
        savedDrive = driveId;
        savedExpires = expiresAt;
    }
    auto& store = nxkit::AuthStore::instance();
    if (!savedAccess.empty())
        store.set("cloud.aliyun.access_token", savedAccess);
    if (!savedRefresh.empty())
        store.set("cloud.aliyun.refresh_token", savedRefresh);
    store.set("cloud.aliyun.expires_at", std::to_string(savedExpires));
    if (!savedDrive.empty()) store.set("cloud.aliyun.drive_id", savedDrive);
}

void AliyunDrive::clearSession() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        refreshToken.clear();
        accessToken.clear();
        driveId.clear();
        expiresAt = 0;
        markers.clear();
        loadedCounts.clear();
    }
    auto& store = nxkit::AuthStore::instance();
    store.remove("cloud.aliyun.refresh_token");
    store.remove("cloud.aliyun.access_token");
    store.remove("cloud.aliyun.expires_at");
    store.remove("cloud.aliyun.drive_id");
    store.remove("cloud.aliyun.nickname");
}

std::string AliyunDrive::tokenValue() {
    std::lock_guard<std::mutex> lock(mutex);
    return accessToken;
}

std::string AliyunDrive::driveIdValue() {
    std::lock_guard<std::mutex> lock(mutex);
    return driveId;
}

bool AliyunDrive::ensureToken(std::string& token, std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!accessToken.empty() && expiresAt > nowMs() + 60000) {
            token = accessToken;
            return true;
        }
    }

    std::string refresh;
    {
        std::lock_guard<std::mutex> lock(mutex);
        refresh = refreshToken;
    }
    if (refresh.empty()) {
        error = "未登录";
        return false;
    }

    nxkit::HttpClient client;
    client.setBaseUrl(kAuthBase);
    client.setHeader("User-Agent", kUserAgent);
    client.setHeader("Referer", kReferer);
    client.setTimeout(20000);
    nxkit::HttpResult result = client.postJson(
        "/v2/account/token", {},
        {{"grant_type", "refresh_token"}, {"refresh_token", refresh}});
    if (!result.ok) {
        error = result.error.empty()
                    ? ("http " + std::to_string(result.status))
                    : result.error;
        netDebugLog("aliyun: token refresh failed " + error);
        return false;
    }

    json root = result.json();
    std::string access = root.is_discarded() ? "" : root.value("access_token", "");
    if (access.empty()) {
        error = apiError(root);
        if (error.empty()) error = "token refresh failed";
        netDebugLog("aliyun: token refresh rejected " + error);
        // The refresh token itself is gone: force a new QR login.
        clearSession();
        return false;
    }

    std::string nextRefresh = root.value("refresh_token", refresh);
    std::string drive = root.value("default_drive_id", "");
    saveSession(access, nextRefresh,
                nowMs() + (int64_t)root.value("expires_in", 7200) * 1000, drive);
    token = access;
    netDebugLog("aliyun: token refreshed");
    return true;
}

// -----------------------------------------------------------------------------
// QR code login
// -----------------------------------------------------------------------------

void AliyunDrive::qrToken(
    std::function<void(const std::string& qrContent, const std::string& token,
                       const std::string& error)> callback) {
    runAsync([callback] {
        nxkit::HttpClient client;
        client.setBaseUrl(kPassportBase);
        client.setHeader("User-Agent", kUserAgent);
        client.setHeader("Referer", kReferer);
        client.setHeader("Origin", "https://www.aliyundrive.com");
        client.setTimeout(20000);
        // Synchronous post inside our own worker: cpr's PostCallback never
        // fired on Switch (the UI sat on "requesting QR" forever).
        nxkit::HttpResult result =
            client.post("/newlogin/qrcode/generate.do", kQrFields, kQrFields);
        {
            char line[400];
            snprintf(line, sizeof(line),
                     "aliyun: qrToken ok=%d code=%d status=%ld error=%s body=%.160s",
                     (int)result.ok, result.code, result.status,
                     result.error.c_str(), result.body.c_str());
            netDebugLog(line);
        }
        if (!result.ok)
            return callback("", "", result.error.empty() ? "network error"
                                                         : result.error);
        json root = result.json();
        json content = root.value("content", json::object());
        json data = content.value("data", json::object());
        // "t" is a JSON number here, so it must not be read as a string.
        std::string t;
        if (data.contains("t")) {
            if (data["t"].is_number())
                t = std::to_string(data["t"].get<long long>());
            else if (data["t"].is_string())
                t = data["t"].get<std::string>();
        }
        std::string ck = data.value("ck", "");
        std::string url = data.value("qrCodeUrl", "");
        if (url.empty()) url = data.value("codeContent", "");
        if (t.empty() || ck.empty() || url.empty()) {
            std::string message = content.value("message", "");
            netDebugLog("aliyun: qrToken failed body=" + result.body.substr(0, 200));
            return callback("", "", message.empty() ? "二维码获取失败" : message);
        }
        netDebugLog("aliyun: qrToken ok");
        callback(url, t + "|" + ck, "");
    });
}

void AliyunDrive::qrPoll(
    const std::string& token,
    std::function<void(QrStatus status, const std::string& error)> callback) {
    size_t bar = token.find('|');
    if (bar == std::string::npos) return callback(QrStatus::Expired, "");
    const std::string t = token.substr(0, bar);
    const std::string ck = token.substr(bar + 1);

    nxkit::Query fields = kQrFields;
    fields.push_back({"t", t});
    fields.push_back({"ck", ck});

    runAsync([this, fields, callback] {
        nxkit::HttpClient client;
        client.setBaseUrl(kPassportBase);
        client.setHeader("User-Agent", kUserAgent);
        client.setHeader("Referer", kReferer);
        client.setHeader("Origin", "https://www.aliyundrive.com");
        client.setTimeout(20000);
        nxkit::HttpResult result =
            client.post("/newlogin/qrcode/query.do", fields, fields);
        if (!result.ok) return callback(QrStatus::Waiting, "");

        json root = result.json();
        json data =
            root.value("content", json::object()).value("data", json::object());
        std::string state = data.value("qrCodeStatus", "");
        netDebugLog("aliyun: qr state " + state);

        if (state.empty() || state == "NEW")
            return callback(QrStatus::Waiting, "");
        if (state == "SCANED") return callback(QrStatus::Scanned, "");
        if (state == "EXPIRED") return callback(QrStatus::Expired, "");
        if (state != "CONFIRMED") return callback(QrStatus::Waiting, "");

        std::string rawBiz = data.value("bizExt", "");
        // The payload is GBK encoded, so it must be transcoded before a JSON
        // parser (which validates UTF-8) can touch it.
        std::string biz = cloud::subtitleToUtf8(base64Decode(rawBiz));
        json login = json::parse(biz, nullptr, false);

        std::string access, refresh, drive, nick;
        if (!login.is_discarded()) {
            json pds = login.value("pds_login_result", json::object());
            if (pds.is_string())
                pds = json::parse(pds.get<std::string>(), nullptr, false);
            if (pds.is_object()) {
                access = pds.value("access_token", pds.value("accessToken", ""));
                refresh =
                    pds.value("refresh_token", pds.value("refreshToken", ""));
                drive = pds.value("default_drive_id",
                                  pds.value("defaultDriveId", ""));
                nick = pds.value("nick_name", pds.value("nickName", ""));
                if (nick.empty()) nick = pds.value("user_name", "");
            }
        }

        // Fallback: plain field extraction, tolerant to payloads the strict
        // parser still rejects. The web client uses camelCase keys.
        if (access.empty() || refresh.empty()) {
            access = jsonField(biz, "accessToken");
            if (access.empty()) access = jsonField(biz, "access_token");
            refresh = jsonField(biz, "refreshToken");
            if (refresh.empty()) refresh = jsonField(biz, "refresh_token");
            if (drive.empty()) {
                drive = jsonField(biz, "defaultDriveId");
                if (drive.empty()) drive = jsonField(biz, "default_drive_id");
            }
            if (nick.empty()) {
                nick = jsonField(biz, "nickName");
                if (nick.empty()) nick = jsonField(biz, "nick_name");
                if (nick.empty()) nick = jsonField(biz, "userName");
            }
            netDebugLog("aliyun: biz fallback access=" +
                        std::to_string(access.size()) + " refresh=" +
                        std::to_string(refresh.size()) + " jsonok=" +
                        std::to_string(!login.is_discarded()));
        }
        if (access.empty() || refresh.empty()) {
            netDebugLog("aliyun: bizExt unusable declen=" +
                        std::to_string(biz.size()) + " dec=" + biz.substr(0, 600));
            return callback(QrStatus::Waiting, "");
        }

        // Already on a worker thread: no need to hand off again.
        // expires_in is not part of the QR payload; the first api call
        // refreshes the token anyway.
        saveSession(access, refresh, 0, drive);
        if (!nick.empty()) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                nickname = nick;
            }
            nxkit::AuthStore::instance().set("cloud.aliyun.nickname", nick);
        }
        refreshIdentity(access);
        netDebugLog("aliyun: login stored");
        callback(QrStatus::Success, "");
    });
}

void AliyunDrive::refreshIdentity(const std::string& token) {
    nxkit::HttpClient client;
    client.setBaseUrl(kApiBase);
    client.setHeader("User-Agent", kUserAgent);
    client.setHeader("Referer", kReferer);
    client.setBearerToken(token);
    client.setHeader("x-canary", "client=web,app=adrive,version=v4.10.0");
    client.setTimeout(20000);
    // /v2/user/get only answers POST; a GET comes back 400/405.
    nxkit::HttpResult result = client.postJson("/v2/user/get", {}, json::object());
    if (!result.ok) {
        netDebugLog("aliyun: identity failed status=" +
                    std::to_string(result.status) + " body=" +
                    result.body.substr(0, 200) + " err=" + result.error);
        return;
    }
    json root = result.json();
    if (root.is_discarded()) return;

    std::string name = root.value("name", "");
    if (name.empty()) name = root.value("nick_name", "");
    std::string drive = root.value("default_drive_id", "");
    if (drive.empty()) drive = root.value("resource_drive_id", "");

    auto& store = nxkit::AuthStore::instance();
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!name.empty()) nickname = name;
        if (!drive.empty()) driveId = drive;
        if (!name.empty()) store.set("cloud.aliyun.nickname", nickname);
        if (!drive.empty()) store.set("cloud.aliyun.drive_id", driveId);
    }
    netDebugLog("aliyun: identity name=" + name + " drive=" + drive);
}

// -----------------------------------------------------------------------------
// HTTP helpers
// -----------------------------------------------------------------------------

bool AliyunDrive::apiPost(const std::string& path, const json& body,
                          const std::string& token, json& root, long& status,
                          std::string& error) {
    nxkit::HttpClient client;
    client.setBaseUrl(kApiBase);
    client.setHeader("User-Agent", kUserAgent);
    client.setHeader("Referer", kReferer);
    client.setBearerToken(token);
    client.setHeader("x-canary", "client=web,app=adrive,version=v4.10.0");
    client.setTimeout(25000);
    nxkit::HttpResult result = client.postJson(path, {}, body);
    status = result.status;
    if (!result.ok) {
        netDebugLog("aliyun: post " + path + " failed status=" +
                    std::to_string(result.status) + " body=" +
                    result.body.substr(0, 300) + " err=" + result.error);
        root = result.json();
        error = result.error.empty() ? ("http " + std::to_string(result.status))
                                     : result.error;
        return false;
    }
    root = result.json();
    if (root.is_discarded()) {
        error = "invalid response";
        return false;
    }
    error = apiError(root);
    return error.empty();
}

std::string AliyunDrive::markerFor(const std::string& dirId, int page) {
    const std::string key = dirId.empty() ? "root" : dirId;
    std::lock_guard<std::mutex> lock(mutex);
    if (page <= 1) {
        markers[key].clear();
        loadedCounts[key] = 0;
        return "";
    }
    size_t index = (size_t)(page - 2);
    if (index >= markers[key].size()) return "";
    return markers[key][index];
}

void AliyunDrive::storeMarker(const std::string& dirId, int page,
                              const std::string& nextMarker) {
    if (nextMarker.empty()) return;
    const std::string key = dirId.empty() ? "root" : dirId;
    std::lock_guard<std::mutex> lock(mutex);
    auto& list = markers[key];
    if (list.size() == (size_t)(page - 1))
        list.push_back(nextMarker);
}

// -----------------------------------------------------------------------------
// Account
// -----------------------------------------------------------------------------

void AliyunDrive::accountInfo(
    std::function<void(const AccountInfo& info, const std::string& error)>
        callback) {
    runAsync([this, callback] {
        AccountInfo info;
        std::string token;
        std::string error;
        if (!ensureToken(token, error)) return callback(info, error);
        {
            std::lock_guard<std::mutex> lock(mutex);
            info.nickname = nickname;
        }

        nxkit::HttpClient client;
        client.setBaseUrl(kApiBase);
        client.setHeader("User-Agent", kUserAgent);
        client.setHeader("Referer", kReferer);
        client.setBearerToken(token);
        client.setHeader("x-canary", "client=web,app=adrive,version=v4.10.0");
        client.setTimeout(20000);
        nxkit::HttpResult result =
            client.postJson("/v2/user/get", {}, json::object());
        if (!result.ok) {
            netDebugLog("aliyun: account failed status=" +
                        std::to_string(result.status) + " body=" +
                        result.body.substr(0, 200));
            return callback(info, result.error);
        }
        json root = result.json();
        if (root.is_discarded()) return callback(info, "invalid response");

        if (info.nickname.empty())
            info.nickname = root.value("name", "");
        info.avatarUrl = root.value("avatar", "");
        json space = root.value("personal_space_info", json::object());
        info.usedCapacity = jsonSize(space.value("used_size", json()));
        info.totalCapacity = jsonSize(space.value("total_size", json()));
        if (info.totalCapacity == 0) {
            // /v2/user/get does not carry the quota, the drive object does.
            std::string drive = driveIdValue();
            if (drive.empty()) {
                refreshIdentity(token);
                drive = driveIdValue();
            }
            nxkit::HttpResult quota = client.postJson(
                "/v2/drive/get", {}, {{"drive_id", drive}});
            netDebugLog("aliyun: space status=" + std::to_string(quota.status) +
                        " body=" + quota.body.substr(0, 300));
            if (quota.ok) {
                json quotaRoot = quota.json();
                if (!quotaRoot.is_discarded()) {
                    json quotaSpace =
                        quotaRoot.value("personal_space_info", json::object());
                    info.usedCapacity = jsonSize(
                        quotaSpace.contains("used_size")
                            ? quotaSpace.value("used_size", json())
                            : quotaRoot.value("used_size", json()));
                    info.totalCapacity = jsonSize(
                        quotaSpace.contains("total_size")
                            ? quotaSpace.value("total_size", json())
                            : quotaRoot.value("total_size", json()));
                }
            }
        }
        callback(info, "");
    });
}

// -----------------------------------------------------------------------------
// Files
// -----------------------------------------------------------------------------

void AliyunDrive::listFiles(
    const std::string& dirId, int page, int pageSize,
    std::function<void(const std::vector<DriveFile>& files, int total,
                       const std::string& error)> callback) {
    runAsync([this, dirId, page, pageSize, callback] {
        const std::string parent = dirId.empty() ? "root" : dirId;
        std::string token;
        std::string error;
        if (!ensureToken(token, error))
            return callback(std::vector<DriveFile>{}, -1, error);

        std::string drive = driveIdValue();
        if (drive.empty()) {
            refreshIdentity(token);
            drive = driveIdValue();
        }
        if (drive.empty())
            return callback(std::vector<DriveFile>{}, -1,
                            "无法获取网盘 ID，请重新登录");

        const std::string marker = markerFor(parent, page);
        if (page > 1 && marker.empty()) {
            int loaded = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                loaded = loadedCounts[parent];
            }
            return callback(std::vector<DriveFile>{}, loaded, "");
        }

        auto request = [&](json& root, long& status) {
            json body = {
                {"drive_id", drive},
                {"parent_file_id", parent},
                {"limit", pageSize > 100 ? 100 : pageSize},
                {"marker", marker},
                {"order_by", "updated_at"},
                {"order_direction", "DESC"},
                {"fields", "*"},
                {"url_expire_sec", 14400},
                // Ask for JPEG explicitly: the default thumbnails are WebP and
                // not every decoder path handles them.
                {"image_thumbnail_process",
                 "image/resize,w_400/format,jpg"},
                {"video_thumbnail_process",
                 "video/snapshot,t_1000,f_jpg,w_400"},
            };
            return apiPost("/adrive/v3/file/list", body, token, root, status,
                           error);
        };

        json root;
        long status = 0;
        bool ok = request(root, status);
        if (!ok && (status == 401 || error.find("Token") != std::string::npos)) {
            // Force one refresh and retry: the cached access token went stale.
            {
                std::lock_guard<std::mutex> lock(mutex);
                expiresAt = 0;
            }
            if (!ensureToken(token, error))
                return callback(std::vector<DriveFile>{}, -1, error);
            ok = request(root, status);
        }
        if (!ok) {
            netDebugLog("aliyun: list failed status=" + std::to_string(status) +
                        " error=" + error + " body=" + root.dump().substr(0, 300));
            return callback(std::vector<DriveFile>{}, -1, error);
        }

        std::vector<DriveFile> files;
        for (const auto& item : root.value("items", json::array())) {
            DriveFile file;
            file.id = item.value("file_id", "");
            file.name = item.value("name", "");
            file.isDir = item.value("type", "file") == "folder";
            file.size = jsonSize(item.value("size", json()));
            file.updatedAt = parseIsoTime(item.value("updated_at", ""));
            file.thumb = item.value("thumbnail", "");
            if (item.value("category", std::string()) == "video")
                file.category = 1;
            if (file.id.empty()) continue;
            files.push_back(std::move(file));
        }

        std::string next = root.value("next_marker", "");
        storeMarker(parent, page, next);

        int loaded = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            loadedCounts[parent] += (int)files.size();
            loaded = loadedCounts[parent];
        }
        callback(files, next.empty() ? loaded : -1, "");
    });
}

void AliyunDrive::downloadInfo(
    const std::string& fileId,
    std::function<void(const std::string& url, const std::string& headers,
                       const std::string& error)> callback) {
    runAsync([this, fileId, callback] {
        std::string token;
        std::string error;
        if (!ensureToken(token, error)) return callback("", "", error);

        std::string drive = driveIdValue();
        if (drive.empty()) return callback("", "", "无法获取网盘 ID");

        json body = {{"drive_id", drive},
                     {"file_id", fileId},
                     {"expire_sec", 14400}};
        json root;
        long status = 0;
        if (!apiPost("/v2/file/get_download_url", body, token, root, status,
                     error))
            return callback("", "", error);
        std::string url = root.value("url", "");
        if (url.empty()) url = root.value("cdn_url", "");
        if (url.empty()) url = root.value("download_url", "");
        if (url.empty()) {
            // Restricted files come back with an empty link; the web player
            // falls back to the transcoded HLS playlist instead.
            json infoBody = {{"drive_id", drive},
                             {"file_id", fileId},
                             {"category", "live_transcoding"},
                             {"template_id", ""},
                             {"url_expire_sec", 14400}};
            json info;
            std::string infoError;
            long infoStatus = 0;
            if (apiPost("/v2/file/get_video_preview_play_info", infoBody, token,
                        info, infoStatus, infoError)) {
                json tasks =
                    info.value("video_preview_play_info", json::object())
                        .value("live_transcoding_task_list", json::array());
                for (auto& item : tasks) {
                    std::string candidate = item.value("url", "");
                    if (!candidate.empty()) {
                        url = candidate;
                        break;
                    }
                }
            }
            netDebugLog("aliyun: dl fallback play_info status=" +
                        std::to_string(infoStatus) + " urlLen=" +
                        std::to_string(url.size()) + " err=" + infoError);
        }
        if (url.empty()) {
            std::string keys;
            for (auto& item : root.items()) keys += item.key() + " ";
            netDebugLog("aliyun: dl no url status=" + std::to_string(status) +
                        " urlDump=" + root["url"].dump().substr(0, 80) +
                        " keys=" + keys + " body=" + root.dump().substr(0, 400));
            return callback("", "", "没有可用的下载地址");
        }
        callback(url, playbackHeaders(), "");
    });
}

void AliyunDrive::videoStreams(
    const std::string& fileId,
    std::function<void(const std::vector<VideoStream>& streams,
                       const std::string& defaultResolution,
                       const std::string& error)> callback) {
    runAsync([this, fileId, callback] {
        std::string token;
        std::string error;
        if (!ensureToken(token, error))
            return callback(std::vector<VideoStream>{}, "", error);
        std::string drive = driveIdValue();
        if (drive.empty()) {
            refreshIdentity(token);
            drive = driveIdValue();
        }
        json body = {{"drive_id", drive},
                     {"file_id", fileId},
                     {"category", "live_transcoding"},
                     {"template_id", ""},
                     {"url_expire_sec", 14400}};
        json root;
        long status = 0;
        std::string requestError;
        if (!apiPost("/v2/file/get_video_preview_play_info", body, token, root,
                     status, requestError))
            return callback(std::vector<VideoStream>{}, "", requestError);
        json tasks = root.value("video_preview_play_info", json::object())
                         .value("live_transcoding_task_list", json::array());

        // Highest quality first so the default stream is the best one.
        const std::vector<std::string> order = {"4k", "2k", "fhd", "hd", "sd",
                                                "ld"};
        std::vector<VideoStream> streams;
        for (auto& item : tasks) {
            VideoStream stream;
            stream.url = item.value("url", "");
            if (stream.url.empty()) continue;
            stream.resolution = item.value("template_id", "");
            std::string lower = stream.resolution;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            auto rank = std::find(order.begin(), order.end(), lower);
            stream.width = rank != order.end()
                               ? (int)(order.end() - rank) * 1000
                               : 0;
            stream.height = 0;
            streams.push_back(stream);
        }
        std::stable_sort(streams.begin(), streams.end(),
                         [](const VideoStream& a, const VideoStream& b) {
                             return a.width > b.width;
                         });
        for (auto& stream : streams) stream.width = 0;  // rank only
        callback(streams, streams.empty() ? "" : streams.front().resolution, "");
    });
}

std::string AliyunDrive::playbackHeaders() const {
    // One header per line: the user agent contains commas.
    return std::string("Referer: ") + kReferer + "\r\nUser-Agent: " +
           kUserAgent;
}

// -----------------------------------------------------------------------------
// File operations
// -----------------------------------------------------------------------------

void AliyunDrive::makeDir(const std::string& parentId, const std::string& name,
                          SimpleCallback callback) {
    runAsync([this, parentId, name, callback] {
        std::string token;
        std::string error;
        if (!ensureToken(token, error)) return callback(error);
        std::string drive = driveIdValue();
        if (drive.empty()) {
            refreshIdentity(token);
            drive = driveIdValue();
        }
        json root;
        long status = 0;
        json body = {{"drive_id", drive},
                     {"parent_file_id", parentId.empty() ? "root" : parentId},
                     {"name", name},
                     {"type", "folder"},
                     {"check_name_mode", "refuse"}};
        // api.aliyundrive.com only serves the v2 "file" actions; the
        // openFile/* ones belong to openapi.alipan.com and answer 404 here.
        if (!apiPost("/adrive/v2/file/create", body, token, root, status,
                     error))
            return callback(error);
        callback("");
    });
}

void AliyunDrive::rename(const std::string& fileId, const std::string& newName,
                         SimpleCallback callback) {
    runAsync([this, fileId, newName, callback] {
        std::string token;
        std::string error;
        if (!ensureToken(token, error)) return callback(error);
        std::string drive = driveIdValue();
        if (drive.empty()) {
            refreshIdentity(token);
            drive = driveIdValue();
        }
        json root;
        long status = 0;
        json body = {
            {"drive_id", drive}, {"file_id", fileId}, {"name", newName}};
        if (!apiPost("/adrive/v2/file/update", body, token, root, status,
                     error))
            return callback(error);
        callback("");
    });
}

void AliyunDrive::remove(const std::vector<std::string>& fileIds,
                         SimpleCallback callback) {
    runAsync([this, fileIds, callback] {
        std::string token;
        std::string error;
        if (!ensureToken(token, error)) return callback(error);
        std::string drive = driveIdValue();
        if (drive.empty()) {
            refreshIdentity(token);
            drive = driveIdValue();
        }
        for (const auto& id : fileIds) {
            json root;
            long status = 0;
            json body = {{"drive_id", drive}, {"file_id", id}};
            if (!apiPost("/v2/recyclebin/trash", body, token, root, status,
                         error)) {
                // Already in the recycle bin (stale row, or the delete was
                // pressed twice): the end state is what the user asked for.
                std::string code =
                    root.is_object() ? root.value("code", "") : "";
                if (code.rfind("NotFound", 0) == 0) continue;
                return callback(error);
            }
            netDebugLog("aliyun: trash status=" + std::to_string(status) +
                        " id=" + id);
        }
        callback("");
    });
}

void AliyunDrive::move(const std::vector<std::string>& fileIds,
                       const std::string& targetDirId,
                       SimpleCallback callback) {
    runAsync([this, fileIds, targetDirId, callback] {
        std::string token;
        std::string error;
        if (!ensureToken(token, error)) return callback(error);
        std::string drive = driveIdValue();
        if (drive.empty()) {
            refreshIdentity(token);
            drive = driveIdValue();
        }
        for (const auto& id : fileIds) {
            json root;
            long status = 0;
            json body = {{"drive_id", drive},
                         {"file_id", id},
                         {"to_parent_file_id",
                          targetDirId.empty() ? "root" : targetDirId},
                         {"check_name_mode", "refuse"}};
            if (!apiPost("/adrive/v2/file/move", body, token, root, status,
                         error))
                return callback(error);
        }
        callback("");
    });
}

// -----------------------------------------------------------------------------
// Upload (web API: file/create -> PUT parts -> file/complete)
// -----------------------------------------------------------------------------

void AliyunDrive::uploadFile(const std::string& parentId, const std::string& name,
                             const std::string& localPath,
                             UploadCallback progress, SimpleCallback callback) {
    uint64_t total = localFileSize(localPath);
    FILE* probe = fopen(localPath.c_str(), "rb");
    if (!probe) return callback("cloud/upload/file_missing");
    fclose(probe);

    runAsync([this, parentId, name, localPath, total, progress, callback] {
        std::string token;
        std::string error;
        if (!ensureToken(token, error)) return callback(error);
        std::string drive = driveIdValue();
        if (drive.empty()) {
            refreshIdentity(token);
            drive = driveIdValue();
        }
        if (drive.empty()) return callback("cloud/upload/failed");

        // create needs the whole-file SHA1 for rapid upload / integrity.
        std::string hash =
            total == 0 ? crypto::sha1Hex("") : crypto::sha1FileHex(localPath);
        if (hash.empty()) return callback("cloud/upload/read_failed");

        json root;
        long status = 0;
        json firstPart = json::object(
            {{"part_number", 1}, {"part_size", total}});
        json body = {{"drive_id", drive},
                     {"parent_file_id", parentId.empty() ? "root" : parentId},
                     {"name", name},
                     {"type", "file"},
                     {"size", total},
                     {"content_hash", hash},
                     {"content_hash_name", "sha1"},
                     {"proof_version", "v1"},
                     {"check_name_mode", "auto_rename"},
                     {"part_info_list", json::array({firstPart})}};
        // v2 is the endpoint this API revision actually exposes (the v1
        // openFile alias answers 404); keep it as a fallback for older hosts.
        if (!apiPost("/adrive/v2/file/create", body, token, root, status,
                     error) &&
            status == 404)
            apiPost("/adrive/v1.0/openFile/create", body, token, root, status,
                    error);
        if (root.is_discarded() || status < 200 || status >= 300)
            return callback(isAuthError(error) ? error
                                               : "cloud/upload/failed");
        netDebugLog("aliyun: create status=" + std::to_string(status) +
                    " body=" + snippet(root.dump()));

        const std::string fileId = root.value("file_id", "");
        const std::string uploadId = root.value("upload_id", "");
        if (root.value("rapid_upload", false) || uploadId.empty()) {
            if (progress) progress(total, total);
            return callback("");
        }

        json parts = root.value("part_info_list", json::array());
        if (parts.empty()) return callback("cloud/upload/failed");

        // The part URL is signed with an empty Content-Type, so sending one
        // makes OSS reject the request with 403 SignatureDoesNotMatch.
        const std::string headerBlock;
        for (const json& part : parts) {
            std::string url = part.value("upload_url", "");
            if (url.empty()) continue;
            uint64_t number = part.value("part_number", 1);
            // Parts are 1-based and each carries its own number; the API
            // normally hands back one part for a single-request upload.
            uint64_t partSize = part.value("part_size", (uint64_t)0);
            if (partSize == 0)
                partSize = (total + parts.size() - 1) / parts.size();
            uint64_t offset = (number - 1) * partSize;
            UploadResponse response;
            bool ok = uploadFileRaw(
                url, "PUT", headerBlock, localPath,
                [progress, total](uint64_t sent, uint64_t) {
                    if (progress && total > 0) progress(sent, total);
                },
                &response, offset, partSize);
            // 409 means the part is already stored server side.
            if (!ok && response.status != 409) {
                netDebugLog("aliyun: part " + std::to_string(number) +
                            " PUT status=" + std::to_string(response.status) +
                            " body=" + snippet(response.body));
                return callback("cloud/upload/failed");
            }
        }

        json complete;
        long completeStatus = 0;
        json completeBody = {
            {"drive_id", drive}, {"file_id", fileId}, {"upload_id", uploadId}};
        if (!apiPost("/adrive/v2/file/complete", completeBody, token, complete,
                     completeStatus, error) &&
            completeStatus == 404)
            apiPost("/adrive/v1.0/openFile/complete", completeBody, token,
                    complete, completeStatus, error);
        if (completeStatus < 200 || completeStatus >= 300) {
            netDebugLog("aliyun: complete status=" +
                        std::to_string(completeStatus) + " body=" +
                        snippet(complete.dump()));
            return callback(isAuthError(error) ? error
                                               : "cloud/upload/failed");
        }
        if (progress) progress(total, total);
        callback("");
    });
}

}  // namespace cloud
