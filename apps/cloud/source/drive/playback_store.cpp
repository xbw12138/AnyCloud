//
// cloud: remembers the last playback position of each file.
//

#include "cloud/drive/playback_store.hpp"

#include <cmath>
#include <fstream>

#include <nlohmann/json.hpp>

#include "nxkit/paths.hpp"

namespace cloud {

namespace {

const char* kFileName = "positions.json";

// Ignore the first seconds of a video (usually the intro) and the very end,
// so a finished video does not reopen on its last frame.
const double kMinPosition  = 8.0;
const double kEndThreshold = 30.0;

}  // namespace

PlaybackStore& PlaybackStore::instance() {
    static PlaybackStore store;
    return store;
}

PlaybackStore::PlaybackStore() { this->load(); }

double PlaybackStore::position(const std::string& id) const {
    std::lock_guard<std::mutex> lock(this->mutex);
    auto it = this->entries.find(id);
    return it == this->entries.end() ? 0.0 : it->second.position;
}

double PlaybackStore::duration(const std::string& id) const {
    std::lock_guard<std::mutex> lock(this->mutex);
    auto it = this->entries.find(id);
    return it == this->entries.end() ? 0.0 : it->second.duration;
}

void PlaybackStore::update(const std::string& id, double seconds,
                           double duration) {
    if (id.empty()) return;

    bool drop = seconds < kMinPosition ||
                (duration > 0 && duration - seconds < kEndThreshold);
    if (std::isfinite(seconds) == false) drop = true;

    std::lock_guard<std::mutex> lock(this->mutex);
    bool changed = false;
    if (drop) {
        changed = this->entries.erase(id) > 0;
    } else {
        Entry& slot = this->entries[id];
        // Avoid pointless disk writes for sub-second differences.
        if (std::abs(slot.position - seconds) >= 1.0) {
            slot.position = seconds;
            changed = true;
        }
        if (duration > 0 && std::abs(slot.duration - duration) >= 1.0) {
            slot.duration = duration;
            changed = true;
        }
    }
    if (changed) this->save();
}

void PlaybackStore::load() {
    std::ifstream input(nxkit::dataPath(kFileName));
    if (!input.is_open()) return;

    nlohmann::json root = nlohmann::json::parse(input, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return;

    for (auto it = root.begin(); it != root.end(); ++it) {
        // Older builds stored a bare number; newer ones store both the
        // position and the duration so the recent page can draw a progress bar.
        Entry entry;
        if (it.value().is_number()) {
            entry.position = it.value().get<double>();
        } else if (it.value().is_object()) {
            entry.position = it.value().value("position", 0.0);
            entry.duration = it.value().value("duration", 0.0);
        } else {
            continue;
        }
        if (entry.position > 0) this->entries[it.key()] = entry;
    }
}

void PlaybackStore::save() const {
    nxkit::ensureDataDirectory();

    nlohmann::json root = nlohmann::json::object();
    for (const auto& entry : this->entries)
        root[entry.first] = {{"position", entry.second.position},
                             {"duration", entry.second.duration}};

    std::ofstream output(nxkit::dataPath(kFileName));
    if (!output.is_open()) return;
    output << root.dump();
}

}  // namespace cloud
