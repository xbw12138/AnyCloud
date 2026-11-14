//
// cloud: provider abstraction for cloud drives (Quark, Aliyun, Baidu, ...).
//

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cloud {

enum class MediaKind { None, Video, Audio, Image };

struct DriveFile {
    std::string id;
    std::string name;
    bool isDir = false;
    uint64_t size = 0;
    /// Milliseconds since epoch, 0 when unknown.
    int64_t updatedAt = 0;
    /// Provider specific category hint (Quark: 1 == video).
    int category = 0;
    std::string thumb;
    /// Filled in by the library scanner: the folder this file lives in, so the
    /// home screen can group a folder's videos into one series.
    std::string parentId;
    std::string parentName;

    MediaKind mediaKind() const;
};

struct VideoStream {
    std::string resolution;  // "low", "normal", "high", "super", "2k", "4k"
    std::string url;         // playable m3u8, playbackHeaders() must be sent
    int width = 0;
    int height = 0;
    int64_t bitrate = 0;
    int64_t duration = 0;  // seconds
};

struct AccountInfo {
    std::string nickname;
    std::string memberType;
    uint64_t usedCapacity = 0;
    uint64_t totalCapacity = 0;
};

enum class QrStatus {
    Success,   // login finished, credentials are stored
    Waiting,   // not scanned yet
    Scanned,   // scanned, waiting for confirmation
    Expired,   // qr code expired, request a new one
};

/// A cloud drive backend. All methods are asynchronous: callbacks fire on a
/// worker thread, UI code must marshal back with brls::sync.
class DriveProvider {
public:
    virtual ~DriveProvider() = default;

    /// Stable identifier used in config keys, e.g. "quark".
    virtual std::string id() const = 0;
    /// Human readable name, e.g. "夸克网盘".
    virtual std::string name() const = 0;
    /// Id of the root directory ("0" on Quark, "root" on Aliyun).
    virtual std::string rootId() const { return "0"; }

    /// Loads persisted credentials (AuthStore). Called once at startup.
    virtual void loadCredentials() = 0;
    virtual bool isLoggedIn() const = 0;
    virtual void logout() = 0;

    // --- QR code login -------------------------------------------------------
    /// Returns the content to encode as a QR code plus an opaque poll token.
    virtual void qrToken(
        std::function<void(const std::string& qrContent, const std::string& token,
                           const std::string& error)> callback) = 0;
    virtual void qrPoll(const std::string& token,
                        std::function<void(QrStatus status, const std::string& error)>
                            callback) = 0;

    // --- Account -------------------------------------------------------------
    virtual void accountInfo(
        std::function<void(const AccountInfo& info, const std::string& error)>
            callback) = 0;

    // --- Files ---------------------------------------------------------------
    /// page starts at 1. total is -1 when unknown.
    virtual void listFiles(
        const std::string& dirId, int page, int pageSize,
        std::function<void(const std::vector<DriveFile>& files, int total,
                           const std::string& error)> callback) = 0;

    /// Direct download url of the original file. `headers` contains the extra
    /// http headers (cookie / referer / ua) the player must send.
    virtual void downloadInfo(
        const std::string& fileId,
        std::function<void(const std::string& url, const std::string& headers,
                           const std::string& error)> callback) = 0;

    /// Transcoded video streams, best quality first.
    virtual void videoStreams(
        const std::string& fileId,
        std::function<void(const std::vector<VideoStream>& streams,
                           const std::string& defaultResolution,
                           const std::string& error)> callback) = 0;

    /// Extra http headers the player must attach to every request it makes
    /// for this provider (cookie / referer / user agent). Quark's video CDN
    /// answers 412 Precondition Failed without the cookie, transcoded m3u8
    /// included.
    virtual std::string playbackHeaders() const { return std::string(); }

    // --- File operations -----------------------------------------------------
    using SimpleCallback = std::function<void(const std::string& error)>;
    virtual void makeDir(const std::string& parentId, const std::string& name,
                         SimpleCallback callback) = 0;
    virtual void rename(const std::string& fileId, const std::string& newName,
                        SimpleCallback callback) = 0;
    virtual void remove(const std::vector<std::string>& fileIds,
                        SimpleCallback callback) = 0;
    virtual void move(const std::vector<std::string>& fileIds,
                      const std::string& targetDirId, SimpleCallback callback) = 0;
};

}  // namespace cloud
