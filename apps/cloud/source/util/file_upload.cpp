//
// cloud: libcurl based upload helpers used by the drive providers.
//

#include "cloud/util/file_upload.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

#include <curl/curl.h>

namespace cloud {

namespace {

size_t writeCallback(char* data, size_t size, size_t count, void* userdata) {
    size_t length = size * count;
    static_cast<std::string*>(userdata)->append(data, length);
    return length;
}

int progressCallback(void* userdata, curl_off_t, curl_off_t, curl_off_t total,
                     curl_off_t uploaded) {
    auto* progress = static_cast<const UploadProgressFn*>(userdata);
    if (progress && *progress)
        (*progress)((uint64_t)(uploaded < 0 ? 0 : uploaded),
                    (uint64_t)(total < 0 ? 0 : total));
    return 0;
}

size_t headerCallback(char* data, size_t size, size_t count, void* userdata) {
    auto* headers =
        static_cast<std::map<std::string, std::string>*>(userdata);
    size_t length = size * count;
    std::string line(data, length);
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        for (char& c : name)
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        std::string value = line.substr(colon + 1);
        size_t begin = value.find_first_not_of(" \t");
        if (begin == std::string::npos) {
            value.clear();
        } else {
            value = value.substr(begin);
        }
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n' ||
                                  value.back() == ' '))
            value.pop_back();
        (*headers)[name] = value;
    }
    return length;
}

/// Wires HEADERFUNCTION so UploadResponse::headers is populated.
void captureHeaders(CURL* curl, UploadResponse* response) {
    if (!response) return;
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response->headers);
}

struct curl_slist* buildHeaders(const std::string& headers) {
    struct curl_slist* list = nullptr;
    size_t start = 0;
    while (start < headers.size()) {
        size_t end = headers.find('\n', start);
        std::string line = headers.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!line.empty()) list = curl_slist_append(list, line.c_str());
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return list;
}

}  // namespace

bool uploadFileRaw(const std::string& url, const std::string& method,
                   const std::string& headers, const std::string& filePath,
                   const UploadProgressFn& progress, UploadResponse* response,
                   uint64_t offset, uint64_t length) {
    FILE* file = fopen(filePath.c_str(), "rb");
    if (!file) {
        if (response) response->error = "cannot open local file";
        return false;
    }
    if (fseeko(file, 0, SEEK_END) != 0) {
        fclose(file);
        if (response) response->error = "cannot read local file";
        return false;
    }
    off_t fileSize = ftello(file);
    if (fileSize < 0) fileSize = 0;
    if (offset > (uint64_t)fileSize) offset = (uint64_t)fileSize;
    uint64_t size = (uint64_t)fileSize - offset;
    if (length > 0 && length < size) size = length;
    // The probe above left the cursor at EOF, so the first part needs an
    // explicit rewind too (it used to upload an empty body and stall).
    if (fseeko(file, (off_t)offset, SEEK_SET) != 0) {
        fclose(file);
        if (response) response->error = "cannot seek local file";
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(file);
        if (response) response->error = "cannot initialise curl";
        return false;
    }

    std::string body;
    // PUT bodies below this size are handed to curl as a plain buffer: the
    // streaming path (CURLOPT_UPLOAD) stalls against the Quark object store on
    // the console, the peer answering 100 Continue and never a final status.
    static const uint64_t kMaxBufferedPart = 8ULL * 1024 * 1024;
    std::string payload;
    struct curl_slist* headerList = buildHeaders(headers);
    // curl invents "Content-Type: application/x-www-form-urlencoded" when the
    // body is passed as a buffer. Signed upload URLs (Aliyun OSS) cover that
    // header in the signature, so suppress it unless the caller set one.
    std::string lowered = headers;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);
    if (lowered.find("content-type") == std::string::npos)
        headerList = curl_slist_append(headerList, "Content-Type:");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    // Abort a transfer that makes no progress instead of blocking forever.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    captureHeaders(curl, response);
    if (method == "PUT" && size <= kMaxBufferedPart) {
        payload.assign((size_t)size, '\0');
        if (!payload.empty() &&
            fread(&payload[0], 1, payload.size(), file) != payload.size()) {
            if (response) response->error = "cannot read local file";
            if (headerList) curl_slist_free_all(headerList);
            curl_easy_cleanup(curl);
            fclose(file);
            return false;
        }
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         (curl_off_t)payload.size());
        // No 100-continue dance: some object stores never send the final
        // response once the interim one is out.
        headerList = curl_slist_append(headerList, "Expect:");
    } else if (method == "PUT") {
        curl_easy_setopt(curl, CURLOPT_READDATA, file);
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)size);
        headerList = curl_slist_append(headerList, "Expect:");
    } else {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)size);
        curl_easy_setopt(curl, CURLOPT_READDATA, file);
    }
    if (headerList) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    if (progress) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }

    CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (response) {
        response->status = status;
        response->body = std::move(body);
        if (code != CURLE_OK)
            response->error = curl_easy_strerror(code);
        else if (status < 200 || status >= 300)
            response->error = "http " + std::to_string(status);
    }
    if (headerList) curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    fclose(file);
    return response && response->ok();
}

bool uploadFileMultipart(const std::string& url, const std::string& headers,
                         const std::string& fieldName,
                         const std::string& fileName, const void* data,
                         size_t size, UploadResponse* response) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (response) response->error = "cannot initialise curl";
        return false;
    }
    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, fieldName.c_str());
    curl_mime_filename(part, fileName.c_str());
    curl_mime_data(part, (const char*)data, size);

    std::string body;
    struct curl_slist* headerList = buildHeaders(headers);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    captureHeaders(curl, response);
    if (headerList) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (response) {
        response->status = status;
        response->body = std::move(body);
        if (code != CURLE_OK)
            response->error = curl_easy_strerror(code);
        else if (status < 200 || status >= 300)
            response->error = "http " + std::to_string(status);
    }
    curl_mime_free(mime);
    if (headerList) curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    return response && response->ok();
}

uint64_t localFileSize(const std::string& path) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) return 0;
    return (uint64_t)info.st_size;
}

bool uploadRawBody(const std::string& url, const std::string& method,
                   const std::string& headers, const std::string& body,
                   UploadResponse* response) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (response) response->error = "cannot initialise curl";
        return false;
    }
    std::string reply;
    struct curl_slist* headerList = buildHeaders(headers);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply);
    captureHeaders(curl, response);
    if (headerList) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body.size());

    CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (response) {
        response->status = status;
        response->body = std::move(reply);
        if (code != CURLE_OK)
            response->error = curl_easy_strerror(code);
        else if (status < 200 || status >= 300)
            response->error = "http " + std::to_string(status);
    }
    if (headerList) curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    return response && response->ok();
}

}  // namespace cloud
