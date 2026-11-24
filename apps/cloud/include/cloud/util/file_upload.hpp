//
// cloud: libcurl based upload helpers used by the drive providers.
//

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace cloud {

struct UploadResponse {
    long status = 0;
    std::string body;
    std::string error;
    /// Response headers, keys lowercased (curl folding: the last value wins).
    std::map<std::string, std::string> headers;

    bool ok() const {
        return this->error.empty() && this->status >= 200 &&
               this->status < 300;
    }
};

using UploadProgressFn = std::function<void(uint64_t sent, uint64_t total)>;

/// Streams a local file to `url` with a raw PUT or POST body. `headers` uses
/// the same CRLF separated "Name: value" format as the rest of the app.
/// `offset`/`length` select a byte range; length 0 uploads to the end of the
/// file (needed by the chunked upload APIs).
bool uploadFileRaw(const std::string& url, const std::string& method,
                   const std::string& headers, const std::string& filePath,
                   const UploadProgressFn& progress, UploadResponse* response,
                   uint64_t offset = 0, uint64_t length = 0);

/// Sends one in-memory block as a `multipart/form-data` file part. Used by
/// Baidu's superfile2 endpoint.
bool uploadFileMultipart(const std::string& url, const std::string& headers,
                         const std::string& fieldName,
                         const std::string& fileName, const void* data,
                         size_t size, UploadResponse* response);

/// Sends an in-memory body with a raw PUT or POST. Used by Quark's
/// CompleteMultipartUpload request.
bool uploadRawBody(const std::string& url, const std::string& method,
                   const std::string& headers, const std::string& body,
                   UploadResponse* response);

/// File size in bytes, 0 when the file cannot be opened.
uint64_t localFileSize(const std::string& path);

}  // namespace cloud
