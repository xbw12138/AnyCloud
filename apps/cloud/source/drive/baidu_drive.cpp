//
// cloud: Baidu Netdisk provider (pan.baidu.com), cookie based web API.
//

#include "cloud/drive/baidu_drive.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <sys/types.h>

#include <borealis/core/thread.hpp>

#include "cloud/util/crypto.hpp"
#include "cloud/util/file_upload.hpp"
#include "cpr/async.h"
#include "cpr/util.h"
#include "nxkit/auth_store.hpp"

using json = nlohmann::json;

namespace cloud {

namespace {

const char* kWebUA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
const char* kPanUA =
    "netdisk;P2SP;3.0.0.8;netdisk;11.12.3;ANG-AN00;android-android;10.0;"
    "JSbridge4.4.0;jointBridge;1.1.0;";
const char* kPanBase = "https://pan.baidu.com";
const char* kPassportBase = "https://passport.baidu.com";
const char* kPcsBase = "https://pcs.baidu.com";
const char* kPanAppId = "250528";
const char* kSignMagic = "ebrcUYixaZv2XGu7KIYKxUrqfnOfpDF";

/// Upload diagnostics go to the same on-device log the QR view uses; without
/// this a failed upload only showed a generic toast.
void uploadLog(const std::string& line) {
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
    if (body.size() <= limit) return body;
    return body.substr(0, limit) + "...";
}

/// /api/filemanager and the upload endpoints reject the browser User-Agent
/// with errno -6; they only accept the netdisk (client) identity.
nxkit::HttpClient withPanUA(const nxkit::HttpClient& base) {
    nxkit::HttpClient client = base;
    client.setHeader("User-Agent", kPanUA);
    return client;
}

int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string stripJsonp(const std::string& body) {
    size_t open = body.find('(');
    size_t close = body.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close <= open)
        return body;
    return body.substr(open + 1, close - open - 1);
}

std::string cookieValue(const std::string& cookie, const std::string& name) {
    std::string prefix = name + "=";
    size_t start = 0;
    while (start < cookie.size()) {
        size_t end = cookie.find(';', start);
        std::string item = cookie.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        size_t first = item.find_first_not_of(' ');
        if (first != std::string::npos) item = item.substr(first);
        if (item.compare(0, prefix.size(), prefix) == 0)
            return item.substr(prefix.size());
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return std::string();
}

std::string parentOf(const std::string& path) {
    std::string clean = path;
    while (clean.size() > 1 && clean.back() == '/') clean.pop_back();
    size_t slash = clean.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return "/";
    return clean.substr(0, slash);
}

std::string baseName(const std::string& path) {
    std::string clean = path;
    while (clean.size() > 1 && clean.back() == '/') clean.pop_back();
    size_t slash = clean.find_last_of('/');
    return slash == std::string::npos ? clean : clean.substr(slash + 1);
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir == "/") return "/" + name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string extractUrl(const json& node) {
    if (node.is_string()) return node.get<std::string>();
    if (node.is_array()) {
        for (const json& item : node) {
            std::string url = extractUrl(item);
            if (!url.empty()) return url;
        }
        return "";
    }
    if (node.is_object()) {
        for (const char* key : {"url", "dlink"}) {
            if (!node.contains(key)) continue;
            std::string url = extractUrl(node[key]);
            if (!url.empty()) return url;
        }
    }
    return "";
}

uint64_t numberUInt(const json& value) {
    if (value.is_number_unsigned()) return value.get<uint64_t>();
    if (value.is_number_integer()) return (uint64_t)value.get<int64_t>();
    if (value.is_string()) {
        try {
            return std::stoull(value.get<std::string>());
        } catch (...) {
        }
    }
    return 0;
}

}  // namespace

BaiduDrive::BaiduDrive() {
    http.setTimeout(20000);
    http.setHeader("User-Agent", kWebUA);
    http.setHeader("Referer", "https://pan.baidu.com/");
}

void BaiduDrive::loadCredentials() {
    std::lock_guard<std::mutex> lock(cookieMutex);
    http.setCookie(nxkit::AuthStore::instance().get("cloud.baidu.cookie"));
}

bool BaiduDrive::isLoggedIn() const {
    std::lock_guard<std::mutex> lock(cookieMutex);
    return !cookieValue(http.cookie(), "BDUSS").empty();
}

void BaiduDrive::logout() {
    {
        std::lock_guard<std::mutex> lock(cookieMutex);
        http.setCookie("");
        bdstoken.clear();
        fsIds.clear();
    }
    nxkit::AuthStore::instance().remove("cloud.baidu.cookie");
    nxkit::AuthStore::instance().remove("cloud.baidu.uid");
    nxkit::AuthStore::instance().remove("cloud.baidu.nickname");
}

void BaiduDrive::saveCookie() {
    std::lock_guard<std::mutex> lock(cookieMutex);
    nxkit::AuthStore::instance().set("cloud.baidu.cookie", http.cookie());
}

nxkit::Query BaiduDrive::panQuery(nxkit::Query query) const {
    std::lock_guard<std::mutex> lock(cookieMutex);
    if (!bdstoken.empty()) query.emplace_back("bdstoken", bdstoken);
    return query;
}

void BaiduDrive::ensureBdstoken(
    std::function<void(const std::string& token)> next) {
    std::string cached;
    {
        std::lock_guard<std::mutex> lock(cookieMutex);
        cached = bdstoken;
    }
    // Call back outside the lock: panQuery() takes the same mutex.
    if (!cached.empty()) return next(cached);

    http.getAsync(
        std::string(kPanBase) + "/api/gettemplatevariable",
        {{"fields", "[\"bdstoken\"]"},
         {"clienttype", "0"},
         {"app_id", kPanAppId},
         {"web", "1"}},
        [this, next](const nxkit::HttpResult& result) {
            std::string token;
            if (result.ok) {
                json root = result.json();
                if (root.contains("result") && root["result"].is_object())
                    token = root["result"].value("bdstoken", std::string());
            }
            if (!token.empty()) {
                std::lock_guard<std::mutex> lock(cookieMutex);
                bdstoken = token;
            }
            next(token);
        },
        10000);
}

void BaiduDrive::mergeResponseCookies(const nxkit::HttpResult& result) {
    if (result.cookies.empty()) return;
    // A cleared cookie arrives with an empty value; writing that into the jar
    // would wipe a session that is still valid.
    std::map<std::string, std::string> fresh;
    for (const auto& cookie : result.cookies) {
        if (cookie.second.empty() || cookie.second == "deleted") continue;
        fresh[cookie.first] = cookie.second;
    }
    if (fresh.empty()) return;

    std::string merged;
    {
        std::lock_guard<std::mutex> lock(cookieMutex);
        merged = nxkit::mergeCookies(http.cookie(), fresh);
        if (merged == http.cookie()) return;
        http.setCookie(merged);
    }
    nxkit::AuthStore::instance().set("cloud.baidu.cookie", merged);
}

std::string BaiduDrive::playbackHeaders() const {
    // Newly issued direct links are signed but still validated against the
    // account on some CDN nodes, so the cookie must travel with the request.
    // Keep the browser identity: the netdisk UA is rejected for open-ended
    // stream requests (403). PlayerPage retries with downloadHeaders() when
    // the browser identity still fails.
    std::string jar;
    {
        std::lock_guard<std::mutex> lock(cookieMutex);
        jar = http.cookie();
    }
    std::string headers = "Referer: https://pan.baidu.com/\r\nUser-Agent: " +
                          std::string(kWebUA) + "\r\n";
    if (!jar.empty()) headers += "Cookie: " + jar + "\r\n";
    return headers;
}

std::string BaiduDrive::thumbnailHeaders() const {
    // The thumbnail host validates the account cookie as well.
    return this->downloadHeaders();
}

std::string BaiduDrive::downloadHeaders() const {
    // Plain downloads are served by PCS itself: unlike playback they expect
    // the netdisk UA and the account cookie, otherwise the CDN answers 403.
    std::string jar;
    {
        std::lock_guard<std::mutex> lock(cookieMutex);
        jar = http.cookie();
    }
    std::string headers = "Referer: https://pan.baidu.com/\r\nUser-Agent: " +
                          std::string(kPanUA) + "\r\n";
    if (!jar.empty()) headers += "Cookie: " + jar + "\r\n";
    return headers;
}

// -----------------------------------------------------------------------------
// Login
// -----------------------------------------------------------------------------

void BaiduDrive::qrToken(
    std::function<void(const std::string&, const std::string&,
                       const std::string&)> callback) {
    http.getAsync(
        std::string(kPassportBase) + "/v2/api/getqrcode",
        {{"lp", "pc"}, {"qrloginfrom", "pc"}, {"tpl", "netdisk"}},
        [callback](const nxkit::HttpResult& result) {
            if (!result.ok) return callback("", "", result.error);
            json root = result.json();
            if (root.value("errno", -1) != 0)
                return callback("", "", "baidu: qrcode request failed");
            std::string image = root.value("imgurl", std::string());
            std::string sign = root.value("sign", std::string());
            if (image.rfind("http", 0) != 0) image = "https://" + image;
            if (image.empty() || sign.empty())
                return callback("", "", "baidu: bad qrcode response");
            callback(image, sign, "");
        });
}

void BaiduDrive::qrPoll(
    const std::string& token,
    std::function<void(QrStatus, const std::string&)> callback) {
    if (token.empty()) return callback(QrStatus::Expired, "");

    http.getAsync(std::string(kPassportBase) + "/channel/unicast",
                  {{"channel_id", token},
                   {"callback", ""},
                   {"tpl", "netdisk"},
                   {"_", std::to_string(nowSeconds())}},
                  [this, callback](const nxkit::HttpResult& result) {
                      if (!result.ok) return callback(QrStatus::Waiting, "");
                      json root =
                          json::parse(stripJsonp(result.body), nullptr, false);
                      if (root.is_discarded() || root.value("errno", -1) != 0)
                          return callback(QrStatus::Waiting, "");
                      json channel = json::parse(
                          root.value("channel_v", std::string()), nullptr, false);
                      if (channel.is_discarded())
                          return callback(QrStatus::Waiting, "");
                      std::string bdussTemp =
                          channel.value("v", std::string());
                      if (bdussTemp.empty()) {
                          int status = channel.value("status", 0);
                          if (status == 1)
                              return callback(QrStatus::Scanned, "");
                          if (status == 2)
                              return callback(QrStatus::Expired, "");
                          return callback(QrStatus::Waiting, "");
                      }

                      http.setFollowRedirects(false);
                      nxkit::HttpResult login = http.get(
                          std::string(kPassportBase) +
                              "/v3/login/main/qrbdusslogin",
                          {{"bduss", bdussTemp},
                           {"u", "https://pan.baidu.com/"},
                           {"tpl", "netdisk"},
                           {"apiver", "v3"},
                           {"tt", std::to_string(nowSeconds() * 1000)}});
                      http.setFollowRedirects(true);
                      if (!login.ok)
                          return callback(QrStatus::Waiting, login.error);

                      std::string jar = nxkit::mergeCookies(
                          http.cookie(), login.cookies);
                      if (login.setCookie.find("BDUSS=") != std::string::npos &&
                          login.cookies.find("BDUSS") == login.cookies.end()) {
                          // Fallback for servers that only expose raw headers.
                          size_t begin = login.setCookie.find("BDUSS=");
                          size_t end = login.setCookie.find(';', begin);
                          jar += "; BDUSS=" +
                                 login.setCookie.substr(
                                     begin + 6, end == std::string::npos
                                                    ? std::string::npos
                                                    : end - begin - 6);
                      }
                      if (cookieValue(jar, "BDUSS").empty())
                          return callback(QrStatus::Expired, "");

                      {
                          std::lock_guard<std::mutex> lock(cookieMutex);
                          http.setCookie(jar);
                      }
                      saveCookie();

                      http.getAsync(
                          std::string(kPanBase) + "/rest/2.0/xpan/nas",
                          {{"method", "uinfo"}, {"lang", "zh"}},
                          [callback](const nxkit::HttpResult& info) {
                              if (info.ok) {
                                  json user = info.json();
                                  std::string uid =
                                      std::to_string(numberUInt(user.value(
                                          "uk", json(0))));
                                  if (uid != "0")
                                      nxkit::AuthStore::instance().set(
                                          "cloud.baidu.uid", uid);
                                  std::string name =
                                      user.value("netdisk_name", std::string());
                                  if (name.empty())
                                      name =
                                          user.value("baidu_name", std::string());
                                  if (!name.empty())
                                      nxkit::AuthStore::instance().set(
                                          "cloud.baidu.nickname", name);
                              }
                              callback(QrStatus::Success, "");
                          });
                  });
}

// -----------------------------------------------------------------------------
// Account
// -----------------------------------------------------------------------------

void BaiduDrive::accountInfo(
    std::function<void(const AccountInfo&, const std::string&)> callback) {
    http.getAsync(
        std::string(kPanBase) + "/rest/2.0/xpan/nas",
        {{"method", "uinfo"}, {"lang", "zh"}},
        [this, callback](const nxkit::HttpResult& result) {
            this->mergeResponseCookies(result);
            if (!result.ok) return callback({}, result.error);
            json root = result.json();
            if (root.value("errno", -1) != 0)
                return callback({}, "baidu: not logged in");

            AccountInfo info;
            info.nickname = root.value("netdisk_name", std::string());
            if (info.nickname.empty())
                info.nickname = root.value("baidu_name", std::string());
            info.avatarUrl = root.value("avatar_url", std::string());
            int vipType = root.value("vip_type", 0);
            if (vipType == 2)
                info.memberType = "超级会员";
            else if (vipType == 1)
                info.memberType = "会员";

            std::string uid = std::to_string(numberUInt(root.value("uk", json(0))));
            if (uid != "0")
                nxkit::AuthStore::instance().set("cloud.baidu.uid", uid);
            if (!info.nickname.empty())
                nxkit::AuthStore::instance().set("cloud.baidu.nickname",
                                                 info.nickname);

            this->ensureBdstoken(
                [this, callback, info](const std::string&) mutable {
                    this->http.getAsync(
                        std::string(kPanBase) + "/api/quota",
                        this->panQuery(
                            {{"checkfree", "1"}, {"checkexpire", "1"}}),
                        [callback, info](const nxkit::HttpResult& quota) mutable {
                            if (quota.ok) {
                                json q = quota.json();
                                if (q.value("errno", -1) == 0) {
                                    info.totalCapacity =
                                        numberUInt(q.value("total", json(0)));
                                    info.usedCapacity =
                                        numberUInt(q.value("used", json(0)));
                                }
                            }
                            callback(info, "");
                        });
                });
        });
}

// -----------------------------------------------------------------------------
// Files
// -----------------------------------------------------------------------------

void BaiduDrive::listFiles(
    const std::string& dirId, int page, int pageSize,
    std::function<void(const std::vector<DriveFile>&, int,
                       const std::string&)> callback) {
    std::string dir = dirId.empty() ? "/" : dirId;
    this->ensureBdstoken([this, dir, page, pageSize, callback](
                             const std::string&) {
    this->http.getAsync(
        std::string(kPanBase) + "/api/list",
        this->panQuery({{"dir", dir},
         {"order", "name"},
         {"desc", "0"},
         {"showempty", "0"},
         {"web", "1"},
         {"page", std::to_string(page)},
         {"num", std::to_string(pageSize)},
         {"channel", "chunlei"},
         {"app_id", kPanAppId},
         {"clienttype", "0"},
         {"thumb", "1"}}),
        [this, callback, page, pageSize](const nxkit::HttpResult& result) {
            this->mergeResponseCookies(result);
            if (!result.ok) return callback({}, 0, result.error);
            json root = result.json();
            if (root.value("errno", -1) != 0) {
                // Keep the server wording: "未登录" is what tells the UI the
                // session expired instead of showing a generic failure.
                std::string message = root.value("errmsg", std::string());
                return callback({}, 0, message.empty()
                                            ? "baidu: list failed"
                                            : "baidu: " + message);
            }

            std::vector<DriveFile> files;
            if (root.contains("list") && root["list"].is_array()) {
                for (const json& item : root["list"]) {
                    DriveFile file;
                    file.name = item.value("server_filename", std::string());
                    file.id = item.value("path", std::string());
                    if (file.name.empty() || file.id.empty()) continue;
                    file.isDir = item.value("isdir", 0) == 1;
                    file.parentId = parentOf(file.id);
                    file.size = numberUInt(item.value("size", json(0)));
                    file.updatedAt =
                        (int64_t)numberUInt(item.value("server_mtime", json(0))) *
                        1000;
                    int category = item.value("category", 0);
                    if (category == 1)
                        file.category = 1;
                    else if (category == 2)
                        file.category = 2;
                    else if (category == 3)
                        file.category = 3;
                    if (!file.isDir && item.contains("thumbs")) {
                        json thumbs = item["thumbs"];
                        // Some endpoints inline the thumbs map as a string.
                        if (thumbs.is_string())
                            thumbs = json::parse(thumbs.get<std::string>(),
                                                 nullptr, false);
                        if (thumbs.is_object()) {
                            for (const char* key : {"url3", "url2", "url1"}) {
                                std::string url =
                                    thumbs.value(key, std::string());
                                if (!url.empty()) {
                                    file.thumb = url;
                                    break;
                                }
                            }
                        }
                    }
                    if (!file.isDir) {
                        int64_t fsId =
                            (int64_t)numberUInt(item.value("fs_id", json(0)));
                        if (fsId != 0) {
                            std::lock_guard<std::mutex> lock(cookieMutex);
                            fsIds[file.id] = fsId;
                        }
                    }
                    files.push_back(std::move(file));
                }
            }
            // Baidu does not report a total; a short page means "the end".
            int total = -1;
            if ((int)files.size() < pageSize)
                total = (page - 1) * pageSize + (int)files.size();
            callback(files, total, "");
        });
    });
}

int64_t BaiduDrive::lookupFsId(const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(cookieMutex);
        auto it = fsIds.find(path);
        if (it != fsIds.end()) return it->second;
    }
    nxkit::HttpResult result = http.get(
        std::string(kPanBase) + "/api/list",
        panQuery({{"dir", parentOf(path)},
                  {"order", "name"},
                  {"desc", "0"},
                  {"showempty", "0"},
                  {"web", "1"},
                  {"page", "1"},
                  {"num", "1000"},
                  {"channel", "chunlei"},
                  {"app_id", kPanAppId},
                  {"clienttype", "0"}}));
    if (!result.ok) return 0;
    json root = result.json();
    if (root.value("errno", -1) != 0 || !root.contains("list")) return 0;
    for (const json& item : root["list"]) {
        if (item.value("path", std::string()) != path) continue;
        int64_t fsId = (int64_t)numberUInt(item.value("fs_id", json(0)));
        if (fsId != 0) {
            std::lock_guard<std::mutex> lock(cookieMutex);
            fsIds[path] = fsId;
        }
        return fsId;
    }
    return 0;
}

void BaiduDrive::ensureUid(
    std::function<void(bool, const std::string&)> done) {
    if (!nxkit::AuthStore::instance().get("cloud.baidu.uid").empty())
        return done(true, "");
    http.getAsync(std::string(kPanBase) + "/rest/2.0/xpan/nas",
                  {{"method", "uinfo"}, {"lang", "zh"}},
                  [done](const nxkit::HttpResult& result) {
                      if (!result.ok) return done(false, result.error);
                      json root = result.json();
                      if (root.value("errno", -1) != 0)
                          return done(false, "baidu: not logged in");
                      std::string uid = std::to_string(
                          numberUInt(root.value("uk", json(0))));
                      if (uid == "0") return done(false, "baidu: no user id");
                      nxkit::AuthStore::instance().set("cloud.baidu.uid", uid);
                      done(true, "");
                  });
}

void BaiduDrive::downloadInfo(
    const std::string& fileId,
    std::function<void(const std::string&, const std::string&,
                       const std::string&)> callback) {
    ensureUid([this, fileId, callback](bool ok, const std::string& error) {
        if (!ok) return callback("", "", error);
        this->requestDownloadUrl(fileId, callback);
    });
}

void BaiduDrive::requestDownloadUrl(
    const std::string& path,
    std::function<void(const std::string&, const std::string&,
                       const std::string&)> callback) {
    std::string bduss;
    std::string cookie;
    std::string uid = nxkit::AuthStore::instance().get("cloud.baidu.uid");
    {
        std::lock_guard<std::mutex> lock(cookieMutex);
        cookie = http.cookie();
        bduss = cookieValue(cookie, "BDUSS");
    }
    if (bduss.empty() || uid.empty())
        return callback("", "", "baidu: not logged in");

    std::string devUid = crypto::md5Hex(bduss) + "|0";
    for (char& c : devUid)
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');

    std::string time = std::to_string(nowSeconds());
    std::string rand = crypto::sha1Hex(crypto::sha1Hex(bduss) + uid +
                                       kSignMagic + time + devUid);

    nxkit::Query query = {
        {"ant", "1"},
        {"check_blue", "1"},
        {"es", "1"},
        {"esl", "1"},
        {"app_id", kPanAppId},
        {"method", "locatedownload"},
        {"path", path},
        {"ver", "4.0"},
        {"clienttype", "17"},
        {"channel", "0"},
        {"apn_id", "1_0"},
        {"freeisp", "0"},
        {"queryfree", "0"},
        {"use", "0"},
        {"time", time},
        {"rand", rand},
        {"devuid", devUid},
        {"cuid", devUid},
    };

    // PCS requires the netdisk user agent; use a dedicated client so the web
    // UA used by the list APIs is not used for the download request.
    nxkit::HttpClient pcs;
    pcs.setTimeout(20000);
    pcs.setHeader("User-Agent", kPanUA);
    pcs.setCookie(cookie);
    pcs.postAsync(std::string(kPcsBase) + "/rest/2.0/pcs/file", query, {},
                  [this, callback](const nxkit::HttpResult& result) {
                      // Do not merge cookies here: PCS answers with its own
                      // cookie scope, and writing it into the web jar overwrote
                      // BDUSS, which made every later pan.baidu.com call fail
                      // with 400 "未登录" (playing a video broke the session).
                      if (!result.ok) return callback("", "", result.error);
                      json root = json::parse(result.body, nullptr, false);
                      if (root.is_discarded())
                          return callback("", "", "baidu: bad download response");
                      if (root.contains("errno") &&
                          root.value("errno", 0) != 0) {
                          std::string message =
                              root.value("errmsg", std::string());
                          return callback("", "",
                                          message.empty()
                                              ? "baidu: download request failed"
                                              : "baidu: " + message);
                      }
                      std::string url;
                      if (root.contains("urls")) url = extractUrl(root["urls"]);
                      if (url.empty() && root.contains("dlink"))
                          url = extractUrl(root["dlink"]);
                      if (url.empty() && root.contains("url"))
                          url = extractUrl(root["url"]);
                      if (url.empty())
                          return callback("", "", "baidu: empty download url");
                      callback(url, this->downloadHeaders(), "");
                  });
}

void BaiduDrive::videoStreams(
    const std::string&,
    std::function<void(const std::vector<VideoStream>&, const std::string&,
                       const std::string&)> callback) {
    callback({}, "", "baidu: no transcoded streams");
}

// -----------------------------------------------------------------------------
// File operations
// -----------------------------------------------------------------------------

void BaiduDrive::makeDir(const std::string& parentId, const std::string& name,
                         SimpleCallback callback) {
    ensureBdstoken([this, parentId, name, callback](const std::string& token) {
        if (token.empty()) return callback("baidu: not logged in");
        nxkit::HttpClient pan;
        {
            std::lock_guard<std::mutex> lock(cookieMutex);
            pan = withPanUA(http);
        }
        pan.postAsync(std::string(kPanBase) + "/api/create",
                      panQuery({{"a", "commit"},
                                {"channel", "chunlei"},
                                {"web", "1"},
                                {"app_id", kPanAppId},
                                {"clienttype", "0"}}),
                      {{"path", joinPath(parentId, name)},
                       {"isdir", "1"},
                       {"block_list", "[]"}},
                      [callback](const nxkit::HttpResult& result) {
                          if (!result.ok) return callback(result.error);
                          json root = json::parse(result.body, nullptr, false);
                          if (root.is_discarded() || root.value("errno", -1) != 0)
                              return callback("baidu: create folder failed");
                          callback("");
                      });
    });
}

void BaiduDrive::rename(const std::string& fileId, const std::string& newName,
                        SimpleCallback callback) {
    // Renaming needs the numeric fs_id, so resolve it on a worker first.
    cpr::async([this, fileId, newName, callback] {
        int64_t fsId = lookupFsId(fileId);
        if (fsId == 0)
            return brls::sync([callback] { callback("baidu: file not found"); });
        json item = {{"id", fsId}, {"path", fileId}, {"newname", newName}};
        ensureBdstoken([this, item, callback](const std::string& token) {
            if (token.empty())
                return brls::sync([callback] {
                    callback("baidu: not logged in");
                });
            nxkit::HttpClient pan;
            {
                std::lock_guard<std::mutex> lock(cookieMutex);
                pan = withPanUA(http);
            }
            nxkit::HttpResult result = pan.post(
                std::string(kPanBase) + "/api/filemanager",
                panQuery({{"opera", "rename"},
                          {"async", "2"},
                          {"onnest", "fail"},
                          {"channel", "chunlei"},
                          {"web", "1"},
                          {"app_id", kPanAppId},
                          {"clienttype", "0"}}),
                {{"filelist", json::array({item}).dump()}});
            std::string error;
            if (!result.ok)
                error = result.error;
            else {
                json root = json::parse(result.body, nullptr, false);
                if (root.is_discarded() || root.value("errno", -1) != 0)
                    error = "baidu: rename failed";
            }
            brls::sync([callback, error] { callback(error); });
        });
    });
}

void BaiduDrive::remove(const std::vector<std::string>& fileIds,
                        SimpleCallback callback) {
    // Delete wants the paths as a plain string array (objects are refused),
    // plus the netdisk identity and a bdstoken: without the token the API
    // answers errno 8001.
    json list = json::array();
    for (const auto& path : fileIds) list.push_back(path);
    ensureBdstoken([this, list, callback](const std::string& token) {
        if (token.empty()) return callback("baidu: not logged in");
        nxkit::HttpClient pan;
        {
            std::lock_guard<std::mutex> lock(cookieMutex);
            pan = withPanUA(http);
        }
        pan.postAsync(std::string(kPanBase) + "/api/filemanager",
                      panQuery({{"opera", "delete"},
                                {"async", "2"},
                                {"onnest", "fail"},
                                {"channel", "chunlei"},
                                {"web", "1"},
                                {"app_id", kPanAppId},
                                {"clienttype", "0"}}),
                      {{"filelist", list.dump()}},
                      [callback](const nxkit::HttpResult& result) {
                          if (!result.ok) return callback(result.error);
                          json root = json::parse(result.body, nullptr, false);
                          if (root.is_discarded() || root.value("errno", -1) != 0)
                              return callback("baidu: delete failed");
                          callback("");
                      });
    });
}

void BaiduDrive::move(const std::vector<std::string>& fileIds,
                      const std::string& targetDirId, SimpleCallback callback) {
    json list = json::array();
    for (const auto& path : fileIds)
        list.push_back({{"path", path},
                        {"dest", targetDirId},
                        {"newname", baseName(path)}});
    ensureBdstoken([this, list, callback](const std::string& token) {
        if (token.empty()) return callback("baidu: not logged in");
        nxkit::HttpClient pan;
        {
            std::lock_guard<std::mutex> lock(cookieMutex);
            pan = withPanUA(http);
        }
        pan.postAsync(std::string(kPanBase) + "/api/filemanager",
                      panQuery({{"opera", "move"},
                                {"async", "2"},
                                {"onnest", "fail"},
                                {"channel", "chunlei"},
                                {"web", "1"},
                                {"app_id", kPanAppId},
                                {"clienttype", "0"}}),
                      {{"filelist", list.dump()}},
                      [callback](const nxkit::HttpResult& result) {
                          if (!result.ok) return callback(result.error);
                          json root = json::parse(result.body, nullptr, false);
                          if (root.is_discarded() || root.value("errno", -1) != 0)
                              return callback("baidu: move failed");
                          callback("");
                      });
    });
}

// -----------------------------------------------------------------------------
// Upload (web API: precreate -> superfile2 blocks -> create)
// -----------------------------------------------------------------------------

void BaiduDrive::uploadFile(const std::string& parentId, const std::string& name,
                            const std::string& localPath, UploadCallback progress,
                            SimpleCallback callback) {
    const uint64_t total = localFileSize(localPath);
    FILE* probe = fopen(localPath.c_str(), "rb");
    if (!probe) return callback("cloud/upload/file_missing");
    fclose(probe);

    const std::string path = joinPath(parentId, name);
    ensureBdstoken([this, parentId, path, localPath, name, total, progress, callback](
                       const std::string& token) {
        if (token.empty()) return callback("cloud/upload/auth");
        cpr::async([this, parentId, path, localPath, name, total, token,
                    progress, callback] {
            // RAPID hashes each 4 MB slice; the same list is sent to create.
            static const uint64_t kBlock = 4ULL * 1024 * 1024;
            const uint64_t blockCount = (total + kBlock - 1) / kBlock;

            json blockList = json::array();
            for (uint64_t i = 0; i < blockCount; i++) {
                uint64_t offset = i * kBlock;
                uint64_t size = std::min<uint64_t>(kBlock, total - offset);
                std::string md5 = crypto::md5FileHex(localPath, offset, size);
                if (md5.empty()) return callback("cloud/upload/read_failed");
                blockList.push_back(md5);
            }

            const std::string targetPath = path;

            // The upload endpoints only accept the netdisk (client) identity:
            // with the browser UA /api/precreate answers errno -6 (which is
            // "not allowed", not "name taken").
            nxkit::HttpClient uploadHttp;
            {
                std::lock_guard<std::mutex> lock(cookieMutex);
                uploadHttp = http;
            }
            uploadHttp.setHeader("User-Agent", kPanUA);

            nxkit::HttpResult pre = uploadHttp.post(
                std::string(kPanBase) + "/api/precreate",
                {{"method", "precreate"},
                 {"app_id", kPanAppId},
                 {"bdstoken", token},
                 {"clienttype", "0"},
                 {"web", "1"},
                 {"channel", "chunlei"}},
                {{"path", targetPath},
                 {"size", std::to_string(total)},
                 {"isdir", "0"},
                 {"autoinit", "1"},
                 {"block_list", blockList.dump()}});
            if (!pre.ok) {
                uploadLog("baidu: precreate http error " + pre.error);
                return callback("cloud/upload/failed");
            }
            json preRoot = json::parse(pre.body, nullptr, false);
            const int preErrno =
                preRoot.is_discarded() ? -1 : preRoot.value("errno", -1);
            uploadLog("baidu: precreate status=" + std::to_string(pre.status) +
                      " errno=" + std::to_string(preErrno) + " body=" +
                      snippet(pre.body));
            if (preRoot.is_discarded()) return callback("cloud/upload/failed");
            if (preErrno != 0) {
                uploadLog("baidu: precreate refused errno=" +
                          std::to_string(preErrno));
                return callback("cloud/upload/failed");
            }

            const std::string uploadId = preRoot.value("uploadid", "");
            // A non-zero return_type (1 or 2 depending on the API revision)
            // means the server already had every block (rapid upload).
            if (uploadId.empty()) {
                if (preRoot.value("return_type", 0) != 0) {
                    if (progress) progress(total, total);
                    return callback("");
                }
                return callback("cloud/upload/failed");
            }

            std::string cookie;
            {
                std::lock_guard<std::mutex> lock(cookieMutex);
                cookie = http.cookie();
            }
            const std::string headerBlock =
                "Cookie: " + cookie + "\r\nUser-Agent: " + kPanUA +
                "\r\nReferer: https://pan.baidu.com/disk/main\r\n"
                "Origin: https://pan.baidu.com\r\n";
            const std::string uploadBase =
                std::string("https://d.pcs.baidu.com") +
                "/rest/2.0/pcs/superfile2?method=upload&app_id=" + kPanAppId +
                "&bdstoken=" + token +
                "&channel=chunlei&clienttype=0&web=1&uploadsign=0&path=" +
                cpr::util::urlEncode(targetPath) + "&uploadid=" + uploadId;

            uint64_t uploaded = 0;
            for (const json& item : preRoot.value("block_list", json::array())) {
                if (!item.is_number_integer()) continue;
                int index = item.get<int>();
                uint64_t offset = (uint64_t)index * kBlock;
                if (offset >= total) continue;
                uint64_t size = std::min<uint64_t>(kBlock, total - offset);

                std::string block((size_t)size, '\0');
                FILE* file = fopen(localPath.c_str(), "rb");
                if (!file) return callback("cloud/upload/read_failed");
                bool readOk =
                    fseeko(file, (off_t)offset, SEEK_SET) == 0 &&
                    fread(&block[0], 1, block.size(), file) == block.size();
                fclose(file);
                if (!readOk) return callback("cloud/upload/read_failed");

                UploadResponse response;
                if (!uploadFileMultipart(
                        uploadBase + "&partseq=" + std::to_string(index),
                        headerBlock, "file", "blob", block.data(), block.size(),
                        &response)) {
                    uploadLog("baidu: part " + std::to_string(index) +
                              " failed status=" +
                              std::to_string(response.status) + " body=" +
                              snippet(response.body));
                    return callback("cloud/upload/failed");
                }
                // PCS answers 200 even when a block is rejected, so check the
                // payload: a part without its md5 never reached the drive and
                // the commit below would fail with errno 10.
                json partRoot = json::parse(response.body, nullptr, false);
                bool partOk = !partRoot.is_discarded() &&
                              partRoot.value("errno", 0) == 0 &&
                              partRoot.contains("md5");
                uploadLog("baidu: part " + std::to_string(index) +
                          " status=" + std::to_string(response.status) +
                          " body=" + snippet(response.body));
                if (!partOk) return callback("cloud/upload/failed");
                uploaded += size;
                if (progress) progress(uploaded, total);
            }

            nxkit::HttpResult create = uploadHttp.post(
                std::string(kPanBase) + "/api/create",
                {{"a", "commit"},
                 {"channel", "chunlei"},
                 {"web", "1"},
                 {"app_id", kPanAppId},
                 {"clienttype", "0"},
                 {"isdir", "0"},
                 {"bdstoken", token}},
                {{"path", targetPath},
                 {"size", std::to_string(total)},
                 {"isdir", "0"},
                 {"block_list", blockList.dump()},
                 {"uploadid", uploadId}});
            if (!create.ok) {
                uploadLog("baidu: create http error " + create.error);
                return callback("cloud/upload/failed");
            }
            json createRoot = json::parse(create.body, nullptr, false);
            uploadLog("baidu: create status=" + std::to_string(create.status) +
                      " body=" + snippet(create.body));
            if (createRoot.is_discarded() ||
                createRoot.value("errno", -1) != 0)
                return callback("cloud/upload/failed");

            if (progress) progress(total, total);
            callback("");
        });
    });
}

}  // namespace cloud
