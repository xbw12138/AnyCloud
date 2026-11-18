//
// cloud: 115 drive provider (115.com), cookie based web API.
//

#include "cloud/drive/drive115.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <vector>

#include "cloud/util/crypto.hpp"
#include "nxkit/auth_store.hpp"

using json = nlohmann::json;

namespace cloud {

namespace {

const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
const char* kReferer = "https://115.com/";

bool stateOk(const json& root) {
    if (!root.contains("state")) return false;
    const json& state = root["state"];
    if (state.is_boolean()) return state.get<bool>();
    if (state.is_number()) return state.get<double>() != 0;
    return false;
}

/// On-device diagnostics, same file the login / player code writes to.
void log115(const std::string& line) {
#ifdef __SWITCH__
    FILE* file = fopen("sdmc:/switch/nxkit/qr_debug.log", "a");
#else
    FILE* file = fopen("/tmp/qr_debug.log", "a");
#endif
    if (!file) return;
    fprintf(file, "%s\n", line.c_str());
    fclose(file);
}

std::string snippet(const nxkit::HttpResult& result, size_t max = 300) {
    std::string body = result.body.substr(0, max);
    for (char& c : body)
        if (c == '\n' || c == '\r') c = ' ';
    return body;
}

std::string jsonString(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number()) return value.dump();
    return std::string();
}

uint64_t jsonUInt(const json& value) {
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

/// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
/// algorithm). libnx has no timegm(), so the conversion is done by hand.
int64_t daysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

int64_t parse115Time(const json& value) {
    if (value.is_number()) {
        int64_t seconds = value.get<int64_t>();
        return seconds > 100000000000LL ? seconds : seconds * 1000;
    }
    if (!value.is_string()) return 0;
    std::string text = value.get<std::string>();
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    int fields = sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day,
                        &hour, &minute, &second);
    if (fields < 5 || year < 1970) return 0;
    // 115 returns local (UTC+8) wall clock time.
    int64_t seconds = daysFromCivil(year, (unsigned)month, (unsigned)day) *
                          86400 +
                      hour * 3600 + minute * 60 + second - 8 * 3600;
    return seconds * 1000;
}

std::vector<std::string> split(const std::string& value, char sep) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t end = value.find(sep, start);
        if (end == std::string::npos) {
            parts.push_back(value.substr(start));
            break;
        }
        parts.push_back(value.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

const char* kApiBase = "https://webapi.115.com";
const char* kQrBase = "https://qrcodeapi.115.com";
const char* kPassportBase = "https://passportapi.115.com";
const char* kProApiBase = "https://proapi.115.com";

}  // namespace

Drive115::Drive115() {
    http.setTimeout(20000);
    // The web API rejects requests without a browser identity (405 for some
    // endpoints), so every call carries the same headers as the website.
    http.setHeader("User-Agent", kUserAgent);
    http.setHeader("Referer", kReferer);
}

void Drive115::loadCredentials() {
    std::lock_guard<std::mutex> lock(cookieMutex);
    http.setCookie(nxkit::AuthStore::instance().get("cloud.115.cookie"));
}

bool Drive115::isLoggedIn() const {
    std::lock_guard<std::mutex> lock(cookieMutex);
    return !http.cookie().empty();
}

void Drive115::logout() {
    {
        std::lock_guard<std::mutex> lock(cookieMutex);
        http.setCookie("");
        pickCodes.clear();
    }
    nxkit::AuthStore::instance().remove("cloud.115.cookie");
    nxkit::AuthStore::instance().remove("cloud.115.nickname");
}

void Drive115::saveCookie() {
    std::lock_guard<std::mutex> lock(cookieMutex);
    nxkit::AuthStore::instance().set("cloud.115.cookie", http.cookie());
}

void Drive115::mergeResponseCookies(const nxkit::HttpResult& result) {
    if (result.cookies.empty()) return;
    std::string merged;
    {
        std::lock_guard<std::mutex> lock(cookieMutex);
        merged = nxkit::mergeCookies(http.cookie(), result.cookies);
        if (merged == http.cookie()) return;
        http.setCookie(merged);
    }
    nxkit::AuthStore::instance().set("cloud.115.cookie", merged);
}

std::string Drive115::playbackHeaders() const {
    std::lock_guard<std::mutex> lock(cookieMutex);
    return "Cookie: " + http.cookie() + "\r\nReferer: " + kReferer +
           "\r\nUser-Agent: " + kUserAgent;
}

// -----------------------------------------------------------------------------
// Login
// -----------------------------------------------------------------------------

void Drive115::qrToken(
    std::function<void(const std::string&, const std::string&,
                       const std::string&)> callback) {
    http.getAsync(
        std::string(kQrBase) + "/api/1.0/web/1.0/token", {},
        [callback](const nxkit::HttpResult& result) {
            log115("115: token status=" + std::to_string(result.status) +
                   " code=" + std::to_string(result.code) +
                   " err=" + result.error + " body=" + snippet(result, 160));
            if (!result.ok) return callback("", "", result.error);
            json root = result.json();
            if (!stateOk(root) || !root.contains("data"))
                return callback("", "", "115: token request failed");
            const json& data = root["data"];
            std::string uid = jsonString(data.value("uid", json()));
            std::string time = jsonString(data.value("time", json()));
            std::string sign = jsonString(data.value("sign", json()));
            std::string content = data.value("qrcode", std::string());
            if (content.empty() && (!uid.empty() && !sign.empty()))
                content = "https://115.com/scan/dg-" + uid;
            if (content.empty() || uid.empty() || sign.empty())
                return callback("", "", "115: bad token response");
            callback(content, uid + "|" + time + "|" + sign, "");
        });
}

void Drive115::qrPoll(
    const std::string& token,
    std::function<void(QrStatus, const std::string&)> callback) {
    auto parts = split(token, '|');
    if (parts.size() != 3) return callback(QrStatus::Expired, "");

    nxkit::Query query = {{"uid", parts[0]}, {"time", parts[1]}, {"sign", parts[2]}};
    http.getAsync(std::string(kQrBase) + "/get/status/", query,
                  [this, callback, uid = parts[0]](
                      const nxkit::HttpResult& result) {
                      log115("115: poll status=" + std::to_string(result.status) +
                             " code=" + std::to_string(result.code) +
                             " err=" + result.error +
                             " body=" + snippet(result, 200));
                      if (!result.ok) return callback(QrStatus::Waiting, "");
                      json root = result.json();
                      if (!stateOk(root) || !root.contains("data"))
                          return callback(QrStatus::Waiting, "");
                      int status = root["data"].value("status", 0);
                      if (status == 1) return callback(QrStatus::Scanned, "");
                      if (status == -1 || status == -2)
                          return callback(QrStatus::Expired, "");
                      if (status != 2) return callback(QrStatus::Waiting, "");

                      http.postAsync(
                          std::string(kPassportBase) +
                              "/app/1.0/web/1.0/login/qrcode",
                          {}, {{"account", uid}, {"app", "web"}},
                          [this, callback](const nxkit::HttpResult& login) {
                              log115("115: login status=" +
                                     std::to_string(login.status) +
                                     " code=" + std::to_string(login.code) +
                                     " err=" + login.error +
                                     " body=" + snippet(login, 200));
                              if (!login.ok)
                                  return callback(QrStatus::Waiting,
                                                  login.error);
                              json root = login.json();
                              if (!stateOk(root) || !root.contains("data"))
                                  return callback(QrStatus::Expired, "");
                              const json& data = root["data"];
                              const json& cookies =
                                  data.contains("cookie") ? data["cookie"] : data;
                              std::string jar;
                              if (cookies.is_object()) {
                                  for (auto it = cookies.begin();
                                       it != cookies.end(); ++it) {
                                      std::string value = jsonString(it.value());
                                      if (value.empty()) continue;
                                      jar += it.key() + "=" + value + "; ";
                                  }
                              }
                              if (jar.empty())
                                  return callback(QrStatus::Expired, "");
                              {
                                  std::lock_guard<std::mutex> lock(cookieMutex);
                                  http.setCookie(jar);
                              }
                              saveCookie();
                              callback(QrStatus::Success, "");
                          });
                  });
}

// -----------------------------------------------------------------------------
// Account
// -----------------------------------------------------------------------------

void Drive115::accountInfo(
    std::function<void(const AccountInfo&, const std::string&)> callback) {
    // /user/info now only exposes the login name (a masked phone) on web
    // sessions, so capacity has to come from /user/space. Both run in parallel
    // and report once, keeping whatever part succeeded.
    auto info = std::make_shared<AccountInfo>();
    auto pending = std::make_shared<std::atomic<int>>(2);
    auto error = std::make_shared<std::string>();

    auto finish = [callback, info, pending, error]() {
        if (--(*pending) > 0) return;
        if (info->nickname.empty() && info->totalCapacity == 0)
            return callback({}, error->empty() ? "115: no account data" : *error);
        callback(*info, "");
    };

    auto readSpace = [](const json& space, AccountInfo& out) {
        if (!space.is_object()) return;
        json total = space.value("all_total", json::object());
        out.totalCapacity = jsonUInt(total.value("size", json(0)));
        if (space.contains("all_use")) {
            json used = space.value("all_use", json::object());
            out.usedCapacity = jsonUInt(used.value("size", json(0)));
        } else if (space.contains("all_remain")) {
            json remain = space.value("all_remain", json::object());
            uint64_t freeBytes = jsonUInt(remain.value("size", json(0)));
            out.usedCapacity = out.totalCapacity > freeBytes
                                   ? out.totalCapacity - freeBytes
                                   : 0;
        }
    };

    http.getAsync(std::string(kApiBase) + "/user/info", {},
                  [this, info, error, finish, readSpace](
                      const nxkit::HttpResult& result) {
                      log115("115: user/info status=" +
                             std::to_string(result.status) +
                             " code=" + std::to_string(result.code) +
                             " err=" + result.error +
                             " body=" + snippet(result, 200));
                      this->mergeResponseCookies(result);
                      if (!result.ok) {
                          if (error->empty()) *error = result.error;
                          return finish();
                      }
                      json root = result.json();
                      if (!stateOk(root)) {
                          if (error->empty()) *error = "115: not logged in";
                          return finish();
                      }
                      json data = root.value("data", json());
                      // Older sessions returned a full object, newer ones only
                      // a string with the masked phone.
                      if (data.is_string()) {
                          info->nickname = data.get<std::string>();
                      } else if (data.is_object()) {
                          info->nickname = data.value("user_name", std::string());
                          info->avatarUrl =
                              data.value("user_face_s", std::string());
                          if (info->avatarUrl.empty())
                              info->avatarUrl =
                                  data.value("user_face_m", std::string());
                          if (info->avatarUrl.empty())
                              info->avatarUrl =
                                  data.value("user_face_l", std::string());
                          if (info->avatarUrl.empty())
                              info->avatarUrl =
                                  data.value("user_face", std::string());
                          if (info->avatarUrl.empty())
                              info->avatarUrl =
                                  data.value("user_head", std::string());
                          if (data.contains("vip_info") &&
                              data["vip_info"].is_object())
                              info->memberType = data["vip_info"].value(
                                  "level_name", std::string());
                          if (info->memberType.empty() &&
                              data.value("vip", 0) != 0)
                              info->memberType = "VIP";
                          readSpace(
                              data.value("rt_space_info", json::object()),
                              *info);
                      }
                      if (!info->nickname.empty())
                          nxkit::AuthStore::instance().set(
                              "cloud.115.nickname", info->nickname);
                      finish();
                  });

    http.getAsync(std::string(kApiBase) + "/user/space", {},
                  [this, info, error, finish, readSpace](
                      const nxkit::HttpResult& result) {
                      log115("115: user/space status=" +
                             std::to_string(result.status) +
                             " code=" + std::to_string(result.code) +
                             " err=" + result.error +
                             " body=" + snippet(result, 160));
                      this->mergeResponseCookies(result);
                      if (!result.ok || !stateOk(result.json())) {
                          if (error->empty())
                              *error = result.ok ? "115: space failed"
                                                 : result.error;
                          return finish();
                      }
                      readSpace(result.json().value("data", json::object()),
                                *info);
                      finish();
                  });
}

// -----------------------------------------------------------------------------
// Files
// -----------------------------------------------------------------------------

void Drive115::listFiles(
    const std::string& dirId, int page, int pageSize,
    std::function<void(const std::vector<DriveFile>&, int,
                       const std::string&)> callback) {
    nxkit::Query query = {
        {"aid", "1"},
        {"cid", dirId.empty() ? "0" : dirId},
        {"offset", std::to_string((page - 1) * pageSize)},
        {"limit", std::to_string(pageSize)},
        {"show_dir", "1"},
        {"snap", "0"},
        {"natsort", "0"},
        {"record_open_time", "1"},
        {"format", "json"},
        {"fc_mix", "0"},
        {"o", "user_ptime"},
        {"asc", "0"},
    };
    http.getAsync(std::string(kApiBase) + "/files", query,
                  [this, dirId, page, callback](const nxkit::HttpResult& result) {
                      log115("115: files cid=" + dirId + " page=" +
                             std::to_string(page) + " status=" +
                             std::to_string(result.status) +
                             " code=" + std::to_string(result.code) +
                             " err=" + result.error +
                             " body=" + snippet(result, 160));
                      this->mergeResponseCookies(result);
                      if (!result.ok) return callback({}, 0, result.error);
                      json root = result.json();
                      if (!stateOk(root))
                          return callback({}, 0, "115: list failed");
                      std::vector<DriveFile> files;
                      int total = root.value("count", 0);
                      if (root.contains("data") && root["data"].is_array()) {
                          for (const json& item : root["data"]) {
                              DriveFile file;
                              file.name = item.value("n", std::string());
                              if (file.name.empty()) continue;
                              std::string ico = item.value("ico", std::string());
                              // Folders only carry `cid` and files only `fid`;
                              // the `ico` field is missing for folders now.
                              std::string fid = jsonString(item.value("fid", json()));
                              file.isDir = fid.empty();
                              file.id = file.isDir
                                            ? jsonString(item.value("cid", json()))
                                            : fid;
                              if (file.id.empty()) continue;
                              file.parentId =
                                  file.isDir
                                      ? jsonString(item.value("pid", json()))
                                      : dirId;
                              file.size = jsonUInt(item.value("s", json(0)));
                              file.updatedAt = parse115Time(item.value(
                                  "t", json(std::string())));
                              if (ico == "video" || file.mediaKind() == MediaKind::Video)
                                  file.category = 1;
                              else if (ico == "audio" || file.mediaKind() == MediaKind::Audio)
                                  file.category = 2;
                              else if (ico == "image" || file.mediaKind() == MediaKind::Image)
                                  file.category = 3;
                              if (!file.isDir) {
                                  std::string pc =
                                      item.value("pc", std::string());
                                  if (!pc.empty()) {
                                      std::lock_guard<std::mutex> lock(
                                          cookieMutex);
                                      this->pickCodes[file.id] = pc;
                                  }
                              }
                              files.push_back(std::move(file));
                          }
                      }
                      callback(files, total, "");
                  });
}

void Drive115::downloadInfo(
    const std::string& fileId,
    std::function<void(const std::string&, const std::string&,
                       const std::string&)> callback) {
    {
        std::lock_guard<std::mutex> lock(cookieMutex);
        auto it = pickCodes.find(fileId);
        if (it != pickCodes.end())
            return requestDownloadUrl(it->second, callback);
    }

    http.getAsync(
        std::string(kApiBase) + "/files/get_info", {{"file_id", fileId}},
        [this, fileId, callback](const nxkit::HttpResult& result) {
            this->mergeResponseCookies(result);
            if (!result.ok) return callback("", "", result.error);
            json root = result.json();
            if (!stateOk(root)) return callback("", "", "115: no pickcode");
            json data = root.value("data", json::object());
            std::string pick;
            if (data.is_array() && !data.empty())
                pick = data[0].value("pc", std::string());
            else if (data.is_object())
                pick = data.value("pc", std::string());
            if (pick.empty()) return callback("", "", "115: no pickcode");
            {
                std::lock_guard<std::mutex> lock(cookieMutex);
                pickCodes[fileId] = pick;
            }
            this->requestDownloadUrl(pick, callback);
        });
}

void Drive115::requestDownloadUrl(
    const std::string& pick,
    std::function<void(const std::string&, const std::string&,
                       const std::string&)> callback) {
    auto key = crypto::m115GenerateKey();
    std::string encoded =
        crypto::m115Encode(json{{"pickcode", pick}}.dump(), key);
    std::string timestamp = std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    http.postAsync(
        std::string(kProApiBase) + "/app/chrome/downurl",
        {{"t", timestamp}}, {{"data", encoded}},
        [this, callback, key](const nxkit::HttpResult& result) {
            this->mergeResponseCookies(result);
            if (!result.ok) return callback("", "", result.error);
            json root = result.json();
            if (!stateOk(root) || !root.contains("data"))
                return callback("", "", "115: download request failed");

            std::string decoded;
            if (root["data"].is_string()) {
                decoded = crypto::m115Decode(root["data"].get<std::string>(), key);
            } else {
                decoded = root["data"].dump();
            }
            json data = json::parse(decoded, nullptr, false);
            if (data.is_discarded()) return callback("", "", "115: bad response");

            std::string url;
            if (data.is_object() && data.contains("url")) {
                const json& node = data["url"];
                url = node.is_string() ? node.get<std::string>()
                                       : node.value("url", std::string());
            }
            if (url.empty() && data.is_object()) {
                for (auto it = data.begin(); it != data.end(); ++it) {
                    if (!it.value().is_object()) continue;
                    const json& node = it.value().value("url", json::object());
                    url = node.is_string() ? node.get<std::string>()
                                           : node.value("url", std::string());
                    if (!url.empty()) break;
                }
            }
            if (url.empty()) return callback("", "", "115: empty download url");
            callback(url, playbackHeaders(), "");
        });
}

void Drive115::videoStreams(
    const std::string&,
    std::function<void(const std::vector<VideoStream>&, const std::string&,
                       const std::string&)> callback) {
    callback({}, "", "115: no transcoded streams");
}

// -----------------------------------------------------------------------------
// File operations
// -----------------------------------------------------------------------------

void Drive115::makeDir(const std::string& parentId, const std::string& name,
                       SimpleCallback callback) {
    http.postAsync(std::string(kApiBase) + "/files/add", {},
                   {{"pid", parentId.empty() ? "0" : parentId}, {"cname", name}},
                   [callback](const nxkit::HttpResult& result) {
                       if (!result.ok) return callback(result.error);
                       json root = result.json();
                       callback(stateOk(root) ? "" : "115: create folder failed");
                   });
}

void Drive115::rename(const std::string& fileId, const std::string& newName,
                      SimpleCallback callback) {
    http.postAsync(std::string(kApiBase) + "/files/batch_rename", {},
                   {{"files_ids", fileId},
                    {"files_new_name[" + fileId + "]", newName}},
                   [callback](const nxkit::HttpResult& result) {
                       if (!result.ok) return callback(result.error);
                       json root = result.json();
                       callback(stateOk(root) ? "" : "115: rename failed");
                   });
}

void Drive115::remove(const std::vector<std::string>& fileIds,
                      SimpleCallback callback) {
    nxkit::Query form;
    for (size_t i = 0; i < fileIds.size(); i++)
        form.push_back({"fid[" + std::to_string(i) + "]", fileIds[i]});
    http.postAsync(std::string(kApiBase) + "/files/delete", {}, form,
                   [callback](const nxkit::HttpResult& result) {
                       if (!result.ok) return callback(result.error);
                       json root = result.json();
                       callback(stateOk(root) ? "" : "115: delete failed");
                   });
}

void Drive115::move(const std::vector<std::string>& fileIds,
                    const std::string& targetDirId, SimpleCallback callback) {
    nxkit::Query form = {{"pid", targetDirId}};
    for (size_t i = 0; i < fileIds.size(); i++)
        form.push_back({"fid[" + std::to_string(i) + "]", fileIds[i]});
    http.postAsync(std::string(kApiBase) + "/files/move", {}, form,
                   [callback](const nxkit::HttpResult& result) {
                       if (!result.ok) return callback(result.error);
                       json root = result.json();
                       callback(stateOk(root) ? "" : "115: move failed");
                   });
}

}  // namespace cloud
