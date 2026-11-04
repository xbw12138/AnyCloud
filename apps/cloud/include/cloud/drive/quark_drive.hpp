//
// cloud: Quark drive provider (pan.quark.cn).
//

#pragma once

#include <mutex>

#include "cloud/drive/drive_provider.hpp"
#include "nxkit/http_client.hpp"

namespace cloud {

class QuarkDrive : public DriveProvider {
public:
    QuarkDrive();

    std::string id() const override { return "quark"; }
    std::string name() const override { return "夸克网盘"; }

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

    /// Extra http headers a player must attach when streaming the original
    /// file or a transcoded m3u8 (cookie + referer + user agent).
    std::string playbackHeaders() const override;

    /// Receives the full response json, or an error message.
    using JsonCallback =
        std::function<void(const nlohmann::json& json, const std::string& error)>;

private:
    /// GET/POST against the clouddrive API with the common query params,
    /// merging rotated cookies back into the jar.
    void driveGet(const std::string& path, const nxkit::Query& query,
                  JsonCallback callback);
    void drivePost(const std::string& path, const nlohmann::json& body,
                   JsonCallback callback);

    /// Follows redirects manually, merging every Set-Cookie into the jar.
    void collectCookies(const std::string& url);

    void saveCookie();
    void mergeResponseCookies(const nxkit::HttpResult& result);

    nxkit::HttpClient http;
    mutable std::mutex cookieMutex;
};

}  // namespace cloud
