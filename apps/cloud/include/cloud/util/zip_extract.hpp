//
// cloud: minimal zip extraction (stored + deflate entries).
//

#pragma once

#include <functional>
#include <string>

namespace cloud {

struct ZipExtractResult {
    bool ok = false;
    int files = 0;
    /// Directory the archive was unpacked into (empty when nothing was
    /// extracted).
    std::string outputDir;
    std::string error;
};

/// Extracts `zipPath` into `outputDir`, creating it when missing. Only the
/// plain store and deflate methods are supported; encrypted entries are
/// rejected. `cancelled` is polled between chunks. `progress` receives the
/// number of uncompressed bytes and files written so far.
ZipExtractResult extractZip(
    const std::string& zipPath, const std::string& outputDir,
    const std::function<bool()>& cancelled = std::function<bool()>(),
    const std::function<void(uint64_t doneBytes, uint64_t totalBytes,
                             int doneFiles, int totalFiles,
                             const std::string& name)>& progress =
        std::function<void(uint64_t, uint64_t, int, int, const std::string&)>());

}  // namespace cloud
