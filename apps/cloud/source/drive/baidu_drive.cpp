//
// cloud: Baidu Netdisk provider (pan.baidu.com), cookie based web API.
//

#include "cloud/drive/baidu_drive.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

#include <borealis/core/thread.hpp>

#include "cloud/util/crypto.hpp"
#include "cpr/async.h"
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

void BaiduDrive::mergeResponseCookies(const nxkit::HttpResult& result) {
    if (result.cookies.empty()) return;
    std::string merged;
    {
        std::lock_guard<std::mutex> lock(cookieMutex);
        merged = nxkit::mergeCookies(http.cookie(), result.cookies);
        if (merged == http.cookie()) return;
        http.setCookie(merged);
    }
    nxkit::AuthStore::instance().set("cloud.baidu.cookie", merged);
}

std::string BaiduDrive::playbackHeaders() const {
    // Download links are self-authenticated in the query string, and the CDN
    // rejects the netdisk UA for open-ended stream requests (403), so playback
    // uses a browser identity instead of the API one.
    return "Referer: https://pan.baidu.com/\r\nUser-Agent: " +
           std::string(kWebUA);
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

            this->http.getAsync(
                std::string(kPanBase) + "/api/quota",
                {{"checkfree", "1"}, {"checkexpire", "1"}},
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
}

// -----------------------------------------------------------------------------
// Files
// -----------------------------------------------------------------------------

void BaiduDrive::listFiles(
    const std::string& dirId, int page, int pageSize,
    std::function<void(const std::vector<DriveFile>&, int,
                       const std::string&)> callback) {
    std::string dir = dirId.empty() ? "/" : dirId;
    http.getAsync(
        std::string(kPanBase) + "/api/list",
        {{"dir", dir},
         {"order", "name"},
         {"desc", "0"},
         {"showempty", "0"},
         {"web", "1"},
         {"page", std::to_string(page)},
         {"num", std::to_string(pageSize)},
         {"channel", "chunlei"},
         {"app_id", kPanAppId},
         {"clienttype", "0"},
         {"thumb", "1"}},
        [this, callback, page, pageSize](const nxkit::HttpResult& result) {
            this->mergeResponseCookies(result);
            if (!result.ok) return callback({}, 0, result.error);
            json root = result.json();
            if (root.value("errno", -1) != 0)
                return callback({}, 0, "baidu: list failed");

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
}

int64_t BaiduDrive::lookupFsId(const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(cookieMutex);
        auto it = fsIds.find(path);
        if (it != fsIds.end()) return it->second;
    }
    nxkit::HttpResult result = http.get(
        std::string(kPanBase) + "/api/list",
        {{"dir", parentOf(path)},
         {"order", "name"},
         {"desc", "0"},
         {"showempty", "0"},
         {"web", "1"},
         {"page", "1"},
         {"num", "1000"},
         {"channel", "chunlei"},
         {"app_id", kPanAppId},
         {"clienttype", "0"}});
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
                      this->mergeResponseCookies(result);
                      if (!result.ok) return callback("", "", result.error);
                      json root = json::parse(result.body, nullptr, false);
                      if (root.is_discarded())
                          return callback("", "", "baidu: bad download response");
                      if (root.contains("errno") &&
                          root.value("errno", 0) != 0)
                          return callback("", "", "baidu: download request failed");
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
    http.postAsync(std::string(kPanBase) + "/api/create",
                   {{"a", "commit"},
                    {"channel", "chunlei"},
                    {"web", "1"},
                    {"app_id", kPanAppId},
                    {"clienttype", "0"}},
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
}

void BaiduDrive::rename(const std::string& fileId, const std::string& newName,
                        SimpleCallback callback) {
    // Renaming needs the numeric fs_id, so resolve it on a worker first.
    cpr::async([this, fileId, newName, callback] {
        int64_t fsId = lookupFsId(fileId);
        if (fsId == 0)
            return brls::sync([callback] { callback("baidu: file not found"); });
        json item = {{"id", fsId}, {"path", fileId}, {"newname", newName}};
        nxkit::HttpResult result = http.post(
            std::string(kPanBase) + "/api/filemanager",
            {{"opera", "rename"},
             {"async", "2"},
             {"onnest", "fail"},
             {"channel", "chunlei"},
             {"web", "1"},
             {"app_id", kPanAppId},
             {"clienttype", "0"}},
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
}

void BaiduDrive::remove(const std::vector<std::string>& fileIds,
                        SimpleCallback callback) {
    json list = json::array();
    for (const auto& path : fileIds) list.push_back({{"path", path}});
    http.postAsync(std::string(kPanBase) + "/api/filemanager",
                   {{"opera", "delete"},
                    {"async", "2"},
                    {"onnest", "fail"},
                    {"channel", "chunlei"},
                    {"web", "1"},
                    {"app_id", kPanAppId},
                    {"clienttype", "0"}},
                   {{"filelist", list.dump()}},
                   [callback](const nxkit::HttpResult& result) {
                       if (!result.ok) return callback(result.error);
                       json root = json::parse(result.body, nullptr, false);
                       if (root.is_discarded() || root.value("errno", -1) != 0)
                           return callback("baidu: delete failed");
                       callback("");
                   });
}

void BaiduDrive::move(const std::vector<std::string>& fileIds,
                      const std::string& targetDirId, SimpleCallback callback) {
    json list = json::array();
    for (const auto& path : fileIds)
        list.push_back({{"path", path},
                        {"dest", targetDirId},
                        {"newname", baseName(path)}});
    http.postAsync(std::string(kPanBase) + "/api/filemanager",
                   {{"opera", "move"},
                    {"async", "2"},
                    {"onnest", "fail"},
                    {"channel", "chunlei"},
                    {"web", "1"},
                    {"app_id", kPanAppId},
                    {"clienttype", "0"}},
                   {{"filelist", list.dump()}},
                   [callback](const nxkit::HttpResult& result) {
                       if (!result.ok) return callback(result.error);
                       json root = json::parse(result.body, nullptr, false);
                       if (root.is_discarded() || root.value("errno", -1) != 0)
                           return callback("baidu: move failed");
                       callback("");
                   });
}

}  // namespace cloud
