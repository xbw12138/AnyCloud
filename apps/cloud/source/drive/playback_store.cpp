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
    auto it = this->positions.find(id);
    return it == this->positions.end() ? 0.0 : it->second;
}

void PlaybackStore::update(const std::string& id, double seconds,
                           double duration) {
    if (id.empty()) return;

    bool drop = seconds < kMinPosition ||
                (duration > 0 && duration - seconds < kEndThreshold);
    if (std::isfinite(seconds) == false) drop = true;

    std::lock_guard<std::mutex> lock(this->mutex);
    if (drop) {
        if (this->positions.erase(id) == 0) return;
    } else {
        double& slot = this->positions[id];
        // Avoid pointless disk writes for sub-second differences.
        if (std::abs(slot - seconds) < 1.0) return;
        slot = seconds;
    }
    this->save();
}

void PlaybackStore::load() {
    std::ifstream input(nxkit::dataPath(kFileName));
    if (!input.is_open()) return;

    nlohmann::json root = nlohmann::json::parse(input, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return;

    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!it.value().is_number()) continue;
        double seconds = it.value().get<double>();
        if (seconds > 0) this->positions[it.key()] = seconds;
    }
}

void PlaybackStore::save() const {
    nxkit::ensureDataDirectory();

    nlohmann::json root = nlohmann::json::object();
    for (const auto& entry : this->positions)
        root[entry.first] = entry.second;

    std::ofstream output(nxkit::dataPath(kFileName));
    if (!output.is_open()) return;
    output << root.dump();
}

}  // namespace cloud
