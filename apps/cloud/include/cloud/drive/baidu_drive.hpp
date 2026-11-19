//
// cloud: Baidu Netdisk provider (pan.baidu.com), cookie based web API.
//

#pragma once

#include <mutex>
#include <unordered_map>

#include "cloud/drive/drive_provider.hpp"
#include "nxkit/http_client.hpp"

namespace cloud {

class BaiduDrive : public DriveProvider {
public:
    BaiduDrive();

    std::string id() const override { return "baidu"; }
    std::string name() const override { return "百度网盘"; }
    std::string rootId() const override { return "/"; }

    void loadCredentials() override;
    bool isLoggedIn() const override;
    void logout() override;

    void qrToken(
        std::function<void(const std::string& qrContent, const std::string& token,
                           const std::string& error)> callback) override;
    void qrPoll(const std::string& token,
                std::function<void(QrStatus status, const std::string& error)>
                    callback) override;
    bool qrContentIsImage() const override { return true; }

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
    std::string thumbnailHeaders() const override;
    std::string downloadHeaders() const;

private:
    void saveCookie();
    void mergeResponseCookies(const nxkit::HttpResult& result);
    void ensureUid(std::function<void(bool ok, const std::string& error)> done);
    /// Web APIs reject cookie-only requests with 400 "未登录" once the server
    /// stops trusting them; the browser always sends bdstoken alongside.
    void ensureBdstoken(std::function<void(const std::string& token)> next);
    nxkit::Query panQuery(nxkit::Query query) const;
    void requestDownloadUrl(
        const std::string& path,
        std::function<void(const std::string& url, const std::string& headers,
                           const std::string& error)> callback);
    int64_t lookupFsId(const std::string& path);

    nxkit::HttpClient http;
    mutable std::mutex cookieMutex;
    mutable std::string bdstoken;
    mutable std::unordered_map<std::string, int64_t> fsIds;
};

}  // namespace cloud
