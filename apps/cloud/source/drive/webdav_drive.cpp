//
// cloud: WebDAV drive.
//

#include "cloud/drive/webdav_drive.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

#include <cpr/async.h>

#include "nxkit/auth_store.hpp"

namespace cloud {

namespace {

const char* kKeyUrl = "cloud.webdav.url";
const char* kKeyUser = "cloud.webdav.user";
const char* kKeyPassword = "cloud.webdav.password";
/// Set when the user signs out. The address stays in the store so the form can
/// prefill it, but the drive counts as signed out until the user connects.
const char* kKeySignedOut = "cloud.webdav.signed_out";

std::string lowerCase(const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

/// Directories first, then by name, so the list matches the other drives.
bool entryLess(const WebDavEntry& left, const WebDavEntry& right) {
    if (left.isDir != right.isDir) return left.isDir;
    return lowerCase(left.name) < lowerCase(right.name);
}

}  // namespace

WebDavConfig WebDavDrive::config() const {
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->credentials;
}

bool WebDavDrive::isLoggedIn() const {
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->configured && this->credentials.valid();
}

std::string WebDavDrive::loginError() const { return "not logged in"; }

void WebDavDrive::loadCredentials() {
    nxkit::AuthStore& store = nxkit::AuthStore::instance();
    WebDavConfig config;
    config.url = webDavNormalizeUrl(store.get(kKeyUrl));
    config.user = store.get(kKeyUser);
    config.password = store.get(kKeyPassword);
    const bool signedOut = store.get(kKeySignedOut) == "1";

    std::lock_guard<std::mutex> lock(this->mutex);
    this->credentials = config;
    this->configured = config.valid() && !signedOut;
}

void WebDavDrive::applyConfig(const WebDavConfig& config) {
    WebDavConfig normalized = config;
    normalized.url = webDavNormalizeUrl(config.url);
    normalized.user = config.user;

    nxkit::AuthStore& store = nxkit::AuthStore::instance();
    store.set(kKeyUrl, normalized.url);
    store.set(kKeyUser, normalized.user);
    store.set(kKeyPassword, normalized.password);
    store.remove(kKeySignedOut);

    std::lock_guard<std::mutex> lock(this->mutex);
    this->credentials = normalized;
    this->configured = normalized.valid();
}

void WebDavDrive::logout() {
    // Keep the address and account so the form comes back filled in; only the
    // session ends and the stored "signed out" marker keeps it that way after
    // a restart.
    nxkit::AuthStore::instance().set(kKeySignedOut, "1");
    std::lock_guard<std::mutex> lock(this->mutex);
    this->configured = false;
}

void WebDavDrive::qrToken(
    std::function<void(const std::string&, const std::string&,
                       const std::string&)> callback) {
    callback("", "", "webdav: use the address form");
}

void WebDavDrive::qrPoll(
    const std::string&,
    std::function<void(QrStatus, const std::string&)> callback) {
    callback(QrStatus::Expired, "webdav: use the address form");
}

void WebDavDrive::accountInfo(
    std::function<void(const AccountInfo&, const std::string&)> callback) {
    WebDavConfig config = this->config();
    if (!config.valid()) {
        callback(AccountInfo(), loginError());
        return;
    }
    cpr::async([config, callback] {
        WebDavSession session(config);
        std::string error;
        if (!session.probe(error)) {
            callback(AccountInfo(), error);
            return;
        }
        AccountInfo info;
        info.nickname = config.user.empty() ? "WebDAV" : config.user;
        info.memberType = webDavBasePath(config.url);
        callback(info, "");
    });
}

void WebDavDrive::listFiles(
    const std::string& dirId, int page, int pageSize,
    std::function<void(const std::vector<DriveFile>&, int,
                       const std::string&)> callback) {
    WebDavConfig config = this->config();
    if (!config.valid()) {
        callback({}, -1, loginError());
        return;
    }
    const std::string dir = webDavNormalize(dirId);
    cpr::async([config, dir, page, pageSize, callback] {
        WebDavSession session(config);
        std::vector<WebDavEntry> entries;
        std::string error;
        if (!session.list(dir, entries, error)) {
            callback({}, -1, error);
            return;
        }
        std::sort(entries.begin(), entries.end(), entryLess);

        std::vector<DriveFile> files;
        files.reserve(entries.size());
        for (const WebDavEntry& entry : entries) {
            DriveFile file;
            file.id = webDavJoin(dir, entry.name);
            file.name = entry.name;
            file.isDir = entry.isDir;
            file.size = entry.size;
            file.updatedAt = entry.mtime * 1000;
            file.parentId = dir;
            files.push_back(std::move(file));
        }

        const int total = (int)files.size();
        const int size = pageSize > 0 ? pageSize : 100;
        const int first = (page > 1 ? (page - 1) : 0) * size;
        if (first >= total) {
            callback({}, total, "");
            return;
        }
        const int last = std::min(total, first + size);
        std::vector<DriveFile> slice(files.begin() + first,
                                     files.begin() + last);
        callback(slice, total, "");
    });
}

void WebDavDrive::downloadInfo(
    const std::string& fileId,
    std::function<void(const std::string&, const std::string&,
                       const std::string&)> callback) {
    const WebDavConfig config = this->config();
    if (!config.valid() || !this->isLoggedIn()) {
        callback("", "", loginError());
        return;
    }
    // Nothing to sign or resolve: mpv / curl read the collection url directly,
    // so the player can start without another worker hop.
    callback(webDavUrlFor(config.url, webDavNormalize(fileId)),
             webDavAuthHeader(config), "");
}

void WebDavDrive::videoStreams(
    const std::string&,
    std::function<void(const std::vector<VideoStream>&, const std::string&,
                       const std::string&)> callback) {
    // No transcoding: the player falls back to downloadInfo().
    callback({}, "", "");
}

void WebDavDrive::makeDir(const std::string& parentId, const std::string& name,
                          SimpleCallback callback) {
    WebDavConfig config = this->config();
    if (!config.valid()) {
        callback(loginError());
        return;
    }
    const std::string target = webDavJoin(parentId, name);
    cpr::async([config, target, callback] {
        WebDavSession session(config);
        std::string error;
        if (!session.mkcol(target, error))
            callback(error);
        else
            callback("");
    });
}

void WebDavDrive::rename(const std::string& fileId, const std::string& newName,
                         SimpleCallback callback) {
    WebDavConfig config = this->config();
    if (!config.valid()) {
        callback(loginError());
        return;
    }
    const std::string source = webDavNormalize(fileId);
    const std::string target = webDavJoin(webDavParent(source), newName);
    cpr::async([config, source, target, callback] {
        WebDavSession session(config);
        std::string error;
        if (!session.move(source, target, error))
            callback(error);
        else
            callback("");
    });
}

void WebDavDrive::remove(const std::vector<std::string>& fileIds,
                         SimpleCallback callback) {
    WebDavConfig config = this->config();
    if (!config.valid()) {
        callback(loginError());
        return;
    }
    std::vector<std::string> targets;
    targets.reserve(fileIds.size());
    for (const std::string& id : fileIds) targets.push_back(webDavNormalize(id));

    cpr::async([config, targets, callback] {
        WebDavSession session(config);
        for (const std::string& target : targets) {
            std::string error;
            if (!session.remove(target, error)) {
                callback(error);
                return;
            }
        }
        callback("");
    });
}

void WebDavDrive::move(const std::vector<std::string>& fileIds,
                       const std::string& targetDirId, SimpleCallback callback) {
    WebDavConfig config = this->config();
    if (!config.valid()) {
        callback(loginError());
        return;
    }
    const std::string targetDir = webDavNormalize(targetDirId);
    std::vector<std::string> sources;
    sources.reserve(fileIds.size());
    for (const std::string& id : fileIds) sources.push_back(webDavNormalize(id));

    cpr::async([config, targetDir, sources, callback] {
        WebDavSession session(config);
        for (const std::string& source : sources) {
            const std::string destination =
                webDavJoin(targetDir, webDavBaseName(source));
            std::string error;
            if (!session.move(source, destination, error)) {
                callback(error);
                return;
            }
        }
        callback("");
    });
}

void WebDavDrive::uploadFile(const std::string& parentId,
                             const std::string& name,
                             const std::string& localPath, UploadCallback progress,
                             SimpleCallback callback) {
    WebDavConfig config = this->config();
    if (!config.valid()) {
        callback(loginError());
        return;
    }
    const std::string target = webDavJoin(parentId, name);

    cpr::async([config, target, localPath, progress, callback] {
        WebDavSession session(config);
        std::string error;
        if (!session.put(target, localPath, progress, error))
            callback(error);
        else
            callback("");
    });
}

}  // namespace cloud
