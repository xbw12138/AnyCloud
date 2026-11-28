//
// cloud: WebDAV drive.
//

#pragma once

#include <mutex>
#include <string>

#include "cloud/drive/drive_provider.hpp"
#include "cloud/drive/webdav_session.hpp"

namespace cloud {

/// A generic WebDAV collection (Nextcloud, Synology, Alist, nginx-dav, ...).
/// File ids are paths inside the configured collection ("/" is its root), so
/// rename / move / delete stay trivial, and playback hands mpv the real url
/// plus a basic auth header instead of a local bridge.
class WebDavDrive : public DriveProvider {
public:
    std::string id() const override { return "webdav"; }
    std::string name() const override { return "WebDAV"; }
    std::string rootId() const override { return "/"; }

    void loadCredentials() override;
    bool isLoggedIn() const override;
    void logout() override;

    /// There is no QR flow: LoginPage shows the address form instead.
    void qrToken(
        std::function<void(const std::string& qrContent, const std::string& token,
                           const std::string& error)> callback) override;
    void qrPoll(const std::string& token,
                std::function<void(QrStatus status, const std::string& error)>
                    callback) override;

    void accountInfo(
        std::function<void(const AccountInfo& info, const std::string& error)>
            callback) override;
    void listFiles(
        const std::string& dirId, int page, int pageSize,
        std::function<void(const std::vector<DriveFile>& files, int total,
                           const std::string& error)> callback) override;
    void downloadInfo(
        const std::string& fileId,
        std::function<void(const std::string& url, const std::string& headers,
                           const std::string& error)> callback) override;
    void videoStreams(
        const std::string& fileId,
        std::function<void(const std::vector<VideoStream>& streams,
                           const std::string& defaultResolution,
                           const std::string& error)> callback) override;

    void makeDir(const std::string& parentId, const std::string& name,
                 SimpleCallback callback) override;
    void rename(const std::string& fileId, const std::string& newName,
                SimpleCallback callback) override;
    void remove(const std::vector<std::string>& fileIds,
                SimpleCallback callback) override;
    void move(const std::vector<std::string>& fileIds,
              const std::string& targetDirId, SimpleCallback callback) override;

    bool supportsUpload() const override { return true; }
    void uploadFile(const std::string& parentId, const std::string& name,
                    const std::string& localPath, UploadCallback progress,
                    SimpleCallback callback) override;

    /// Credentials snapshot, safe to read from any thread.
    WebDavConfig config() const;
    /// Persists and activates an account that was just verified by the UI.
    void applyConfig(const WebDavConfig& config);

private:
    /// "not logged in" makes the UI route back to the address form.
    std::string loginError() const;

    mutable std::mutex mutex;
    WebDavConfig credentials;
    bool configured = false;
};

}  // namespace cloud
