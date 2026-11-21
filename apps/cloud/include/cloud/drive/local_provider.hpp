//
// cloud: playback-only provider for files already downloaded to the console.
//

#pragma once

#include "cloud/drive/drive_provider.hpp"

namespace cloud {

/// Adapts local files to the DriveProvider interface so the regular player can
/// open them. A file id is the absolute path on disk, which mpv opens as is.
class LocalDriveProvider : public DriveProvider {
public:
    std::string id() const override { return "local"; }
    std::string name() const override { return "本地文件"; }

    void loadCredentials() override {}
    bool isLoggedIn() const override { return true; }
    void logout() override {}

    void qrToken(
        std::function<void(const std::string&, const std::string&,
                           const std::string&)> callback) override {
        callback("", "", "unsupported");
    }
    void qrPoll(const std::string&,
                std::function<void(QrStatus, const std::string&)> callback)
        override {
        callback(QrStatus::Expired, "unsupported");
    }

    void accountInfo(
        std::function<void(const AccountInfo&, const std::string&)> callback)
        override {
        callback(AccountInfo{}, "unsupported");
    }

    /// Nothing to list: an empty answer also turns off the sidecar subtitle
    /// scan, embedded tracks are still picked up by the player itself.
    void listFiles(
        const std::string&, int, int,
        std::function<void(const std::vector<DriveFile>&, int,
                           const std::string&)> callback) override {
        callback({}, 0, "");
    }

    void downloadInfo(const std::string& fileId,
                      std::function<void(const std::string&, const std::string&,
                                         const std::string&)> callback) override {
        callback(fileId, "", "");
    }

    /// No transcoded variants: the player falls back to downloadInfo().
    void videoStreams(
        const std::string&,
        std::function<void(const std::vector<VideoStream>&, const std::string&,
                           const std::string&)> callback) override {
        callback({}, "", "");
    }

    std::string playbackHeaders() const override { return std::string(); }

    void makeDir(const std::string&, const std::string&,
                 SimpleCallback callback) override {
        callback("unsupported");
    }
    void rename(const std::string&, const std::string&,
                SimpleCallback callback) override {
        callback("unsupported");
    }
    void remove(const std::vector<std::string>&,
                SimpleCallback callback) override {
        callback("unsupported");
    }
    void move(const std::vector<std::string>&, const std::string&,
              SimpleCallback callback) override {
        callback("unsupported");
    }
};

}  // namespace cloud
