//
// cloud: SMB network share drive.
//

#include "cloud/drive/smb_drive.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

#include <cpr/async.h>
#include <fcntl.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include "cloud/drive/smb_bridge.hpp"
#include "nxkit/auth_store.hpp"

namespace cloud {

namespace {

const char* kKeyServer = "cloud.smb.server";
const char* kKeyShare = "cloud.smb.share";
const char* kKeyUser = "cloud.smb.user";
const char* kKeyPassword = "cloud.smb.password";
/// Set when the user signs out. The share details stay in the store so the
/// form can prefill them, but the drive counts as signed out until the user
/// connects again.
const char* kKeySignedOut = "cloud.smb.signed_out";

std::string lowerCase(const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

/// Directories first, then by name, so the list matches the other drives.
bool entryLess(const DriveFile& left, const DriveFile& right) {
    if (left.isDir != right.isDir) return left.isDir;
    return lowerCase(left.name) < lowerCase(right.name);
}

bool writeAll(smb2_context* ctx, smb2fh* file, const char* data, size_t length,
              std::string& error) {
    size_t written = 0;
    while (written < length) {
        int count = smb2_write(ctx, file,
                               (const uint8_t*)data + written,
                               (uint32_t)(length - written));
        if (count <= 0) {
            error = "smb: write failed";
            const char* message = smb2_get_error(ctx);
            if (message && *message) error = std::string("smb: ") + message;
            return false;
        }
        written += (size_t)count;
    }
    return true;
}

}  // namespace

SmbConfig SmbDrive::config() const {
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->credentials;
}

bool SmbDrive::isLoggedIn() const {
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->configured && this->credentials.valid();
}

std::string SmbDrive::loginError() const { return "not logged in"; }

void SmbDrive::loadCredentials() {
    nxkit::AuthStore& store = nxkit::AuthStore::instance();
    SmbConfig config;
    config.server = smbNormalizeServer(store.get(kKeyServer));
    config.share = store.get(kKeyShare);
    config.user = store.get(kKeyUser);
    config.password = store.get(kKeyPassword);
    const bool signedOut = store.get(kKeySignedOut) == "1";

    {
        std::lock_guard<std::mutex> lock(this->mutex);
        this->credentials = config;
        this->configured = config.valid() && !signedOut;
    }
    // The bridge may be asked for a url before the settings page runs again.
    if (this->isLoggedIn()) {
        SmbBridge::instance().setConfig(config);
        // Start the listener together with the restored share, so the first
        // media open after a restart does not have to pay for it.
        SmbBridge::instance().start();
    }
}

void SmbDrive::applyConfig(const SmbConfig& config) {
    SmbConfig normalized = config;
    normalized.server = smbNormalizeServer(config.server);
    normalized.share = smbNormalize(config.share);
    if (normalized.share == "/") normalized.share.clear();
    else if (!normalized.share.empty() && normalized.share[0] == '/')
        normalized.share.erase(normalized.share.begin());

    nxkit::AuthStore& store = nxkit::AuthStore::instance();
    store.set(kKeyServer, normalized.server);
    store.set(kKeyShare, normalized.share);
    store.set(kKeyUser, normalized.user);
    store.set(kKeyPassword, normalized.password);
    store.remove(kKeySignedOut);

    {
        std::lock_guard<std::mutex> lock(this->mutex);
        this->credentials = normalized;
        this->configured = normalized.valid();
    }
    SmbBridge::instance().setConfig(normalized);
}

void SmbDrive::logout() {
    // Keep the share details so the form comes back filled in; only the
    // session ends and the stored "signed out" marker keeps it that way after
    // a restart.
    nxkit::AuthStore::instance().set(kKeySignedOut, "1");
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        this->configured = false;
    }
    SmbBridge::instance().setConfig(SmbConfig());
}

void SmbDrive::qrToken(
    std::function<void(const std::string&, const std::string&,
                       const std::string&)> callback) {
    callback("", "", "smb: use the share form");
}

void SmbDrive::qrPoll(
    const std::string&,
    std::function<void(QrStatus, const std::string&)> callback) {
    callback(QrStatus::Expired, "smb: use the share form");
}

void SmbDrive::accountInfo(
    std::function<void(const AccountInfo&, const std::string&)> callback) {
    SmbConfig config = this->config();
    if (!config.valid()) {
        callback(AccountInfo(), loginError());
        return;
    }
    cpr::async([config, callback] {
        SmbSession session(config);
        std::string error;
        if (!session.connect(error)) {
            callback(AccountInfo(), error);
            return;
        }

        AccountInfo info;
        info.nickname = config.user.empty() ? config.share + " (guest)"
                                            : config.user;
        info.memberType = config.server + " / " + config.share;

        struct smb2_statvfs vfs;
        if (smb2_statvfs(session.handle(), "", &vfs) == 0) {
            uint64_t blockSize = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
            info.totalCapacity = (uint64_t)vfs.f_blocks * blockSize;
            uint64_t free = (uint64_t)vfs.f_bavail * blockSize;
            if (free <= info.totalCapacity)
                info.usedCapacity = info.totalCapacity - free;
        }
        callback(info, "");
    });
}

void SmbDrive::listFiles(
    const std::string& dirId, int page, int pageSize,
    std::function<void(const std::vector<DriveFile>&, int,
                       const std::string&)> callback) {
    SmbConfig config = this->config();
    if (!config.valid()) {
        callback({}, -1, loginError());
        return;
    }
    const std::string dir = smbNormalize(dirId);
    cpr::async([config, dir, page, pageSize, callback] {
        SmbSession session(config);
        std::string error;
        if (!session.connect(error)) {
            callback({}, -1, error);
            return;
        }
        std::vector<SmbEntry> entries;
        if (!session.list(dir, entries, error)) {
            callback({}, -1, error);
            return;
        }

        std::vector<DriveFile> files;
        files.reserve(entries.size());
        for (const SmbEntry& entry : entries) {
            DriveFile file;
            file.id = smbJoin(dir, entry.name);
            file.name = entry.name;
            file.isDir = entry.isDir;
            file.size = entry.size;
            file.updatedAt = entry.mtime * 1000;
            file.parentId = dir;
            files.push_back(std::move(file));
        }
        std::sort(files.begin(), files.end(), entryLess);

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

void SmbDrive::downloadInfo(
    const std::string& fileId,
    std::function<void(const std::string&, const std::string&,
                       const std::string&)> callback) {
    if (!this->isLoggedIn()) {
        callback("", "", loginError());
        return;
    }
    // The share form starts the local bridge, so this is a plain lookup: no
    // worker hop and no thread creation while the player is opening a file.
    SmbBridge& bridge = SmbBridge::instance();
    bridge.setConfig(this->config());
    const std::string url = bridge.urlFor(smbNormalize(fileId));
    if (url.empty()) {
        callback("", "", "smb: local bridge is not running");
        return;
    }
    callback(url, "", "");
}

void SmbDrive::videoStreams(
    const std::string&,
    std::function<void(const std::vector<VideoStream>&, const std::string&,
                       const std::string&)> callback) {
    // No transcoding: the player falls back to downloadInfo().
    callback({}, "", "");
}

void SmbDrive::makeDir(const std::string& parentId, const std::string& name,
                       SimpleCallback callback) {
    SmbConfig config = this->config();
    if (!config.valid()) {
        callback(loginError());
        return;
    }
    const std::string target = smbApiPath(smbJoin(parentId, name));
    cpr::async([config, target, callback] {
        SmbSession session(config);
        std::string error;
        if (!session.connect(error)) {
            callback(error);
            return;
        }
        if (smb2_mkdir(session.handle(), target.c_str()) != 0)
            callback(session.lastError());
        else
            callback("");
    });
}

void SmbDrive::rename(const std::string& fileId, const std::string& newName,
                      SimpleCallback callback) {
    SmbConfig config = this->config();
    if (!config.valid()) {
        callback(loginError());
        return;
    }
    const std::string source = smbApiPath(fileId);
    const std::string target =
        smbApiPath(smbJoin(smbParent(source), newName));
    cpr::async([config, source, target, callback] {
        SmbSession session(config);
        std::string error;
        if (!session.connect(error)) {
            callback(error);
            return;
        }
        if (smb2_rename(session.handle(), source.c_str(), target.c_str()) != 0)
            callback(session.lastError());
        else
            callback("");
    });
}

void SmbDrive::remove(const std::vector<std::string>& fileIds,
                      SimpleCallback callback) {
    SmbConfig config = this->config();
    if (!config.valid()) {
        callback(loginError());
        return;
    }
    std::vector<std::string> targets;
    targets.reserve(fileIds.size());
    for (const std::string& id : fileIds) targets.push_back(smbApiPath(id));

    cpr::async([config, targets, callback] {
        SmbSession session(config);
        std::string error;
        if (!session.connect(error)) {
            callback(error);
            return;
        }
        for (const std::string& target : targets) {
            smb2_context* ctx = session.handle();
            // Files go through unlink, directories need rmdir.
            int rc = smb2_unlink(ctx, target.c_str());
            if (rc != 0) rc = smb2_rmdir(ctx, target.c_str());
            if (rc != 0) {
                callback(session.lastError());
                return;
            }
        }
        callback("");
    });
}

void SmbDrive::move(const std::vector<std::string>& fileIds,
                    const std::string& targetDirId, SimpleCallback callback) {
    SmbConfig config = this->config();
    if (!config.valid()) {
        callback(loginError());
        return;
    }
    const std::string targetDir = smbApiPath(targetDirId);
    std::vector<std::string> targets;
    targets.reserve(fileIds.size());
    for (const std::string& id : fileIds) targets.push_back(smbApiPath(id));

    cpr::async([config, targetDir, targets, callback] {
        SmbSession session(config);
        std::string error;
        if (!session.connect(error)) {
            callback(error);
            return;
        }
        for (const std::string& source : targets) {
            const std::string destination =
                smbApiPath(smbJoin(targetDir, smbBaseName(source)));
            if (smb2_rename(session.handle(), source.c_str(),
                            destination.c_str()) != 0) {
                callback(session.lastError());
                return;
            }
        }
        callback("");
    });
}

void SmbDrive::uploadFile(const std::string& parentId, const std::string& name,
                          const std::string& localPath, UploadCallback progress,
                          SimpleCallback callback) {
    SmbConfig config = this->config();
    if (!config.valid()) {
        callback(loginError());
        return;
    }
    const std::string target = smbApiPath(smbJoin(parentId, name));

    cpr::async([config, target, localPath, progress, callback] {
        FILE* local = fopen(localPath.c_str(), "rb");
        if (!local) {
            callback("cannot open local file");
            return;
        }
        fseek(local, 0, SEEK_END);
        uint64_t total = (uint64_t)ftell(local);
        fseek(local, 0, SEEK_SET);

        SmbSession session(config);
        std::string error;
        if (!session.connect(error)) {
            fclose(local);
            callback(error);
            return;
        }
        smb2_context* ctx = session.handle();
        smb2fh* file = smb2_open(ctx, target.c_str(),
                                 O_WRONLY | O_CREAT | O_TRUNC);
        if (!file) {
            fclose(local);
            callback(session.lastError());
            return;
        }

        std::vector<char> buffer(256 * 1024);
        uint64_t sent = 0;
        bool ok = true;
        while (ok) {
            size_t count = fread(buffer.data(), 1, buffer.size(), local);
            if (count == 0) break;
            if (!writeAll(ctx, file, buffer.data(), count, error)) {
                ok = false;
                break;
            }
            sent += count;
            if (progress) progress(sent, total);
        }
        fclose(local);
        session.closeFile(file);

        if (!ok) {
            // Do not leave a half uploaded file behind.
            smb2_unlink(ctx, target.c_str());
            callback(error.empty() ? "smb: upload failed" : error);
            return;
        }
        if (progress) progress(total, total);
        callback("");
    });
}

}  // namespace cloud
