//
// cloud: locally persisted list of recently opened media files.
//

#pragma once

#include <mutex>
#include <vector>

#include "cloud/drive/drive_provider.hpp"

namespace cloud {

/// Stores the media files opened from the browser (newest first).
/// There is no server-side "recent" API, so the list lives in
/// <data dir>/recent.json and is capped to 50 entries.
class RecentStore {
public:
    static RecentStore& instance();

    /// Recently opened files, newest first.
    std::vector<DriveFile> files() const;

    /// Adds (or moves) `file` to the front of the list.
    void add(const DriveFile& file);

private:
    RecentStore();

    void load();
    void save() const;

    mutable std::mutex mutex;
    std::vector<DriveFile> entries;
};

}  // namespace cloud
