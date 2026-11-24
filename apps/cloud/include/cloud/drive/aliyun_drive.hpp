//
// cloud: Aliyun Drive provider (www.aliyundrive.com, web endpoints).
//

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "cloud/drive/drive_provider.hpp"
#include "nxkit/http_client.hpp"

namespace cloud {

/// Uses the same web endpoints the aliyundrive.com client talks to: QR code
/// login through passport.aliyundrive.com, then bearer tokens refreshed via
/// auth.aliyundrive.com. No developer application is required.
class AliyunDrive : public DriveProvider {
public:
    AliyunDrive();

    std::string id() const override { return "aliyun"; }
    std::string name() const override { return "阿里云盘"; }
    std::string rootId() const override { return "root"; }

    void loadCredentials() override;
    bool isLoggedIn() const override;
    void logout() override;

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

    std::string playbackHeaders() const override;

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

private:
    /// Refreshes the access token when it is missing or about to expire.
    /// Runs on a worker thread.
    bool ensureToken(std::string& token, std::string& error);

    /// POST against api.aliyundrive.com. Fills `root` on success, otherwise
    /// `error` (and `status` keeps the http status for retry decisions).
    bool apiPost(const std::string& path, const nlohmann::json& body,
                 const std::string& token, nlohmann::json& root, long& status,
                 std::string& error);

    /// Reads nickname / drive id from /v2/user/get and stores them.
    void refreshIdentity(const std::string& token);

    void saveSession(const std::string& access, const std::string& refresh,
                     int64_t expiresAt, const std::string& drive);
    void clearSession();

    std::string tokenValue();
    std::string driveIdValue();
    /// Marker that starts the given page (page 1 starts at "").
    std::string markerFor(const std::string& dirId, int page);
    void storeMarker(const std::string& dirId, int page,
                     const std::string& nextMarker);

    mutable std::mutex mutex;
    std::string refreshToken;
    std::string accessToken;
    std::string driveId;
    std::string nickname;
    int64_t expiresAt = 0;

    /// Aliyun paginates with markers instead of offsets.
    std::map<std::string, std::vector<std::string>> markers;
    std::map<std::string, int> loadedCounts;
};

}  // namespace cloud
