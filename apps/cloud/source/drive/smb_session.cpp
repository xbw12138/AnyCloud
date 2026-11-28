//
// cloud: SMB2 connection helpers.
//

#include "cloud/drive/smb_session.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include <fcntl.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

namespace cloud {

namespace {

std::string trim(const std::string& text) {
    size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

}  // namespace

std::string smbNormalize(const std::string& path) {
    std::string value = trim(path);
    if (value.empty() || value == "/") return "/";
    // Accept full smb urls and relative paths alike.
    if (value.compare(0, 6, "smb://") == 0) value = value.substr(6);
    if (!value.empty() && value[0] != '/') value.insert(value.begin(), '/');
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
}

std::string smbApiPath(const std::string& path) {
    std::string value = smbNormalize(path);
    if (value == "/") return std::string();
    while (!value.empty() && value[0] == '/') value.erase(value.begin());
    return value;
}

std::string smbNormalizeServer(const std::string& server) {
    std::string value = trim(server);
    if (value.compare(0, 6, "smb://") == 0) value = value.substr(6);
    // Drop a share or path that was pasted along with the host.
    size_t slash = value.find('/');
    if (slash != std::string::npos) value = value.substr(0, slash);
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

std::string smbJoin(const std::string& dir, const std::string& name) {
    std::string base = smbNormalize(dir);
    if (name.empty()) return base;
    if (base == "/") return "/" + name;
    return base + "/" + name;
}

std::string smbParent(const std::string& path) {
    std::string value = smbNormalize(path);
    size_t slash = value.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return "/";
    return value.substr(0, slash);
}

std::string smbBaseName(const std::string& path) {
    std::string value = smbNormalize(path);
    size_t slash = value.find_last_of('/');
    if (slash == std::string::npos) return value;
    return value.substr(slash + 1);
}

std::string smbEncodePath(const std::string& path) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size());
    for (unsigned char c : path) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~' || c == '/') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string smbDecodePath(const std::string& encoded) {
    std::string out;
    out.reserve(encoded.size());
    for (size_t i = 0; i < encoded.size(); i++) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(encoded[i + 1]);
            int lo = hex(encoded[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(encoded[i]);
    }
    return out;
}

SmbSession::SmbSession(const SmbConfig& config) : credentials(config) {}

SmbSession::~SmbSession() { close(); }

std::string SmbSession::lastError() const {
    if (!ctx) return "smb: not connected";
    const char* message = smb2_get_error(ctx);
    if (!message || !*message) return "smb: unknown error";
    return std::string("smb: ") + message;
}

bool SmbSession::connect(std::string& error) {
    close();

    if (!credentials.valid()) {
        error = "smb: server and share are required";
        return false;
    }

    ctx = smb2_init_context();
    if (!ctx) {
        error = "smb: cannot create context";
        return false;
    }
    smb2_set_timeout(ctx, 30);
    smb2_set_security_mode(ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);
    // An empty user keeps the session anonymous (guest share).
    smb2_set_user(ctx, credentials.user.c_str());
    if (!credentials.user.empty()) {
        smb2_set_password(ctx, credentials.password.c_str());
        smb2_set_domain(ctx, "WORKGROUP");
        smb2_set_workstation(ctx, "AnyCloud");
    }

    int rc = smb2_connect_share(ctx, credentials.server.c_str(),
                                credentials.share.c_str(),
                                credentials.user.empty() ? ""
                                                         : credentials.user.c_str());
    if (rc != 0) {
        error = lastError();
        close();
        return false;
    }
    return true;
}

void SmbSession::close() {
    if (!ctx) return;
    smb2_disconnect_share(ctx);
    smb2_destroy_context(ctx);
    ctx = nullptr;
}

bool SmbSession::list(const std::string& dirPath,
                      std::vector<SmbEntry>& entries, std::string& error) {
    if (!ctx) {
        error = "smb: not connected";
        return false;
    }
    const std::string path = smbApiPath(dirPath);
    struct smb2dir* dir = smb2_opendir(ctx, path.c_str());
    if (!dir) {
        error = lastError();
        return false;
    }
    struct smb2dirent* entry = nullptr;
    while ((entry = smb2_readdir(ctx, dir)) != nullptr) {
        if (!entry->name) continue;
        std::string name = entry->name;
        if (name.empty() || name == "." || name == "..") continue;
        SmbEntry item;
        item.name = std::move(name);
        item.isDir = entry->st.smb2_type == SMB2_TYPE_DIRECTORY;
        item.size = entry->st.smb2_size;
        item.mtime = (int64_t)entry->st.smb2_mtime;
        entries.push_back(std::move(item));
    }
    smb2_closedir(ctx, dir);
    return true;
}

bool SmbSession::stat(const std::string& path, SmbEntry& entry,
                      std::string& error) {
    if (!ctx) {
        error = "smb: not connected";
        return false;
    }
    const std::string target = smbApiPath(path);
    struct smb2_stat_64 st;
    if (smb2_stat(ctx, target.c_str(), &st) != 0) {
        error = lastError();
        return false;
    }
    entry.name = smbBaseName(target);
    entry.isDir = st.smb2_type == SMB2_TYPE_DIRECTORY;
    entry.size = st.smb2_size;
    entry.mtime = (int64_t)st.smb2_mtime;
    return true;
}

smb2fh* SmbSession::openRead(const std::string& path, uint64_t& size,
                             std::string& error) {
    if (!ctx) {
        error = "smb: not connected";
        return nullptr;
    }
    const std::string target = smbApiPath(path);
    struct smb2fh* file = smb2_open(ctx, target.c_str(), O_RDONLY);
    if (!file) {
        error = lastError();
        return nullptr;
    }
    struct smb2_stat_64 st;
    if (smb2_fstat(ctx, file, &st) != 0) {
        error = lastError();
        smb2_close(ctx, file);
        return nullptr;
    }
    size = st.smb2_size;
    return file;
}

int SmbSession::readAt(smb2fh* file, uint64_t offset, void* buffer, size_t len,
                       std::string& error) {
    if (!ctx || !file) {
        error = "smb: not connected";
        return -1;
    }
    int count = smb2_pread(ctx, file, (uint8_t*)buffer, (uint32_t)len, offset);
    if (count < 0) {
        error = lastError();
        return -1;
    }
    return count;
}

void SmbSession::closeFile(smb2fh* file) {
    if (!ctx || !file) return;
    smb2_close(ctx, file);
}

}  // namespace cloud
