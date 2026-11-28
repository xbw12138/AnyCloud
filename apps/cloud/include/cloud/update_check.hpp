//
// cloud: checks the app store for a newer release.
//

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cloud {

struct UpdateInfo {
    /// True when the store version is newer than the running build.
    bool available = false;
    std::string version;
    std::string changelog;
    uint64_t size = 0;
};

/// One published build, as listed by the store.
struct ReleaseNote {
    std::string version;
    std::string changelog;
    /// Release date, "YYYY-MM-DD" (empty when the store did not report one).
    std::string date;
};

/// Compares dotted versions such as "1.6.9"; returns true when `latest` is
/// newer than `current`.
bool isNewerVersion(const std::string& latest, const std::string& current);

/// Queries the store in the background; the callback runs on a worker thread.
void checkForUpdate(
    std::function<void(bool ok, const UpdateInfo& info, const std::string& error)>
        callback);

/// Shows the result dialog. Must be called on the UI thread. Automatic checks
/// stay silent unless there is something newer to install.
void presentUpdateResult(const UpdateInfo& info, bool ok,
                         const std::string& error, bool manual);

/// Reads the store's published version list (newest first). The callback runs
/// on a worker thread.
void fetchReleaseHistory(
    std::function<void(bool ok, const std::vector<ReleaseNote>& notes,
                       const std::string& error)> callback);

}  // namespace cloud
