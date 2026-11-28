//
// cloud: SMB2 connection helpers shared by the smb drive and its http bridge.
//

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct smb2_context;
struct smb2fh;

namespace cloud {

/// Credentials of one SMB share. An empty user means guest / anonymous.
struct SmbConfig {
    std::string server;
    std::string share;
    std::string user;
    std::string password;

    bool valid() const { return !server.empty() && !share.empty(); }
};

/// One directory entry of a share.
struct SmbEntry {
    std::string name;
    bool isDir = false;
    uint64_t size = 0;
    /// Unix seconds, 0 when the server does not report it.
    int64_t mtime = 0;
};

/// Joins a directory and a name ("/" + "a" -> "/a").
std::string smbJoin(const std::string& dir, const std::string& name);
/// Parent directory of a share path ("/a/b" -> "/a", "/a" -> "/").
std::string smbParent(const std::string& path);
std::string smbBaseName(const std::string& path);
/// Drops "smb://", the trailing slash and forces a leading one.
std::string smbNormalize(const std::string& path);
/// libsmb2 wants share relative paths without a leading slash, "" is the root.
std::string smbApiPath(const std::string& path);
/// Host part of a server field, tolerating "smb://host/" style input.
std::string smbNormalizeServer(const std::string& server);

/// Percent encoding for the bridge url (only unreserved characters survive).
std::string smbEncodePath(const std::string& path);
std::string smbDecodePath(const std::string& encoded);

/// One blocking SMB2 session: negotiate, session setup and tree connect.
/// libsmb2 contexts are not thread safe, so every operation opens its own
/// short lived session instead of sharing one behind a lock.
class SmbSession {
public:
    explicit SmbSession(const SmbConfig& config);
    ~SmbSession();

    SmbSession(const SmbSession&) = delete;
    SmbSession& operator=(const SmbSession&) = delete;

    bool connect(std::string& error);
    void close();

    smb2_context* handle() const { return ctx; }
    const SmbConfig& config() const { return credentials; }
    /// Last libsmb2 error, prefixed with "smb: " for the UI.
    std::string lastError() const;

    /// Lists a directory. "." / ".." are skipped, order is server side.
    bool list(const std::string& dirPath, std::vector<SmbEntry>& entries,
              std::string& error);
    bool stat(const std::string& path, SmbEntry& entry, std::string& error);

    /// Opens a file for reading and reports its size. Null on failure.
    smb2fh* openRead(const std::string& path, uint64_t& size,
                     std::string& error);
    /// Reads up to len bytes at offset. Returns the byte count, -1 on error.
    int readAt(smb2fh* file, uint64_t offset, void* buffer, size_t len,
               std::string& error);
    void closeFile(smb2fh* file);

private:
    SmbConfig credentials;
    smb2_context* ctx = nullptr;
};

}  // namespace cloud
