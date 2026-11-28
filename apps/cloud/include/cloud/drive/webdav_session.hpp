//
// cloud: WebDAV client (PROPFIND / MKCOL / MOVE / DELETE / PUT).
//

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cloud {

/// One WebDAV account: the base collection url plus optional basic auth.
struct WebDavConfig {
    /// Collection url without a trailing slash, e.g.
    /// "https://dav.example.com/remote.php/dav/files/me".
    std::string url;
    std::string user;
    std::string password;

    bool valid() const { return !url.empty(); }
    /// Without credentials the server must allow anonymous access.
    bool anonymous() const { return user.empty() && password.empty(); }
};

/// One entry of a collection.
struct WebDavEntry {
    std::string name;
    bool isDir = false;
    uint64_t size = 0;
    /// Unix seconds, 0 when the server does not report it.
    int64_t mtime = 0;
};

/// Trims the field, adds "http://" when the scheme is missing and drops the
/// trailing slash.
std::string webDavNormalizeUrl(const std::string& url);
/// "/a/b" style path inside the collection ("/" is the collection root).
std::string webDavNormalize(const std::string& path);
std::string webDavJoin(const std::string& dir, const std::string& name);
std::string webDavParent(const std::string& path);
std::string webDavBaseName(const std::string& path);
/// Percent encoding that keeps '/' so whole paths can be encoded at once.
std::string webDavEncodePath(const std::string& path);
std::string webDavDecodePath(const std::string& encoded);
/// Absolute url of `path` inside the collection.
std::string webDavUrlFor(const std::string& baseUrl, const std::string& path);
/// "Authorization: Basic ..." line for mpv / curl, empty for anonymous.
std::string webDavAuthHeader(const WebDavConfig& config);
/// Path component of the collection url ("/dav"), "/" for a host root.
std::string webDavBasePath(const std::string& baseUrl);

/// One blocking WebDAV client. libcurl handles are created per request, so a
/// session can be built on the worker thread that uses it and thrown away.
class WebDavSession {
public:
    explicit WebDavSession(const WebDavConfig& config);
    ~WebDavSession() = default;

    WebDavSession(const WebDavSession&) = delete;
    WebDavSession& operator=(const WebDavSession&) = delete;

    /// Depth 1 PROPFIND: the collection itself and its parent are skipped.
    bool list(const std::string& dirPath, std::vector<WebDavEntry>& entries,
              std::string& error);
    /// Depth 0 PROPFIND: cheap "url reachable / credentials accepted" check.
    bool probe(std::string& error);

    bool mkcol(const std::string& path, std::string& error);
    bool remove(const std::string& path, std::string& error);
    bool move(const std::string& from, const std::string& to,
              std::string& error);
    bool put(const std::string& path, const std::string& localPath,
             const std::function<void(uint64_t sent, uint64_t total)>& progress,
             std::string& error);

    /// HTTP status of the last request, 0 when curl itself failed.
    long lastStatus() const { return this->status; }

private:
    struct Response {
        bool ok = false;
        long status = 0;
        std::string body;
        std::string error;
    };

    Response request(const std::string& method, const std::string& path,
                     const std::vector<std::string>& headers,
                     const std::string& body);
    /// Readable message for a failed response, already prefixed with "webdav: ".
    std::string describe(const Response& response) const;

    WebDavConfig credentials;
    long status = 0;
};

}  // namespace cloud
