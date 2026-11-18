//
// cloud: 115 drive provider (115.com), cookie based web API.
//

#pragma once

#include <mutex>
#include <unordered_map>

#include "cloud/drive/drive_provider.hpp"
#include "nxkit/http_client.hpp"

namespace cloud {

class Drive115 : public DriveProvider {
public:
    Drive115();

    std::string id() const override { return "115"; }
    std::string name() const override { return "115网盘"; }
    std::string rootId() const override { return "0"; }

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

    void makeDir(const std::string& parentId, const std::string& name,
                 SimpleCallback callback) override;
    void rename(const std::string& fileId, const std::string& newName,
                SimpleCallback callback) override;
    void remove(const std::vector<std::string>& fileIds,
                SimpleCallback callback) override;
    void move(const std::vector<std::string>& fileIds,
              const std::string& targetDirId, SimpleCallback callback) override;

    std::string playbackHeaders() const override;

private:
    void saveCookie();
    void mergeResponseCookies(const nxkit::HttpResult& result);
    void requestDownloadUrl(
        const std::string& pickCode,
        std::function<void(const std::string& url, const std::string& headers,
                           const std::string& error)> callback);

    nxkit::HttpClient http;
    mutable std::mutex cookieMutex;
    mutable std::unordered_map<std::string, std::string> pickCodes;
};

}  // namespace cloud
