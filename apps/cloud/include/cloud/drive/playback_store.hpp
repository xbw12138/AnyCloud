//
// cloud: remembers the last playback position of each file.
//

#pragma once

#include <map>
#include <mutex>
#include <string>

namespace cloud {

/// Persists "where the user stopped watching" per file id, so reopening a
/// video continues from that point instead of the beginning.
/// The data lives in <data dir>/positions.json.
class PlaybackStore {
public:
    static PlaybackStore& instance();

    /// Seconds remembered for `id`, 0 when unknown.
    double position(const std::string& id) const;

    /// Remembers `seconds` for `id`. Positions at the very beginning or very
    /// close to the end are dropped (there is nothing worth resuming).
    void update(const std::string& id, double seconds, double duration);

private:
    PlaybackStore();

    void load();
    void save() const;

    mutable std::mutex mutex;
    std::map<std::string, double> positions;
};

}  // namespace cloud
