//
// cloud: locally persisted list of recently opened media files.
//

#include "cloud/drive/recent_store.hpp"

#include <fstream>

#include <nlohmann/json.hpp>

#include "cloud/drive/drive_manager.hpp"
#include "nxkit/paths.hpp"

namespace cloud {

namespace {

const size_t kMaxEntries = 50;
const char* kFileName = "recent.json";

}  // namespace

RecentStore& RecentStore::instance() {
    static RecentStore store;
    return store;
}

RecentStore::RecentStore() { this->load(); }

std::vector<DriveFile> RecentStore::files() const {
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->entries;
}

void RecentStore::add(const DriveFile& input) {
    if (input.isDir || input.id.empty() || input.name.empty()) return;

    DriveFile file = input;
    // Keep the drive the entry really came from: replaying an old entry while
    // another drive is active must not relabel it.
    if (file.providerId.empty()) {
        if (auto provider = DriveManager::instance().findByNameOrId(
                file.provider)) {
            file.providerId = provider->id();
            file.provider = provider->name();
        } else if (auto current = DriveManager::instance().current()) {
            file.providerId = current->id();
            file.provider = current->name();
        }
    }

    std::lock_guard<std::mutex> lock(this->mutex);

    for (auto it = this->entries.begin(); it != this->entries.end(); ++it) {
        if (it->id == file.id && it->providerId == file.providerId &&
            it->provider == file.provider) {
            this->entries.erase(it);
            break;
        }
    }

    this->entries.insert(this->entries.begin(), file);
    if (this->entries.size() > kMaxEntries)
        this->entries.resize(kMaxEntries);

    this->save();
}

void RecentStore::load() {
    std::ifstream input(nxkit::dataPath(kFileName));
    if (!input.is_open()) return;

    nlohmann::json root = nlohmann::json::parse(input, nullptr, false);
    if (root.is_discarded() || !root.is_array()) return;

    for (const auto& item : root) {
        if (this->entries.size() >= kMaxEntries) break;
        if (!item.is_object()) continue;

        DriveFile file;
        file.id = item.value("id", "");
        file.name = item.value("name", "");
        file.size = item.value("size", (uint64_t)0);
        file.updatedAt = item.value("modified", (int64_t)0);
        file.thumb = item.value("thumb", "");
        file.parentId = item.value("parent", "");
        file.providerId = item.value("provider_id", "");
        file.provider = item.value("provider", "");
        file.isDir = false;
        if (file.id.empty() || file.name.empty()) continue;
        // Records written before provider ids existed only stored the name.
        if (file.providerId.empty() && !file.provider.empty()) {
            if (auto provider =
                    DriveManager::instance().findByNameOrId(file.provider)) {
                file.providerId = provider->id();
                file.provider = provider->name();
            }
        }
        this->entries.push_back(std::move(file));
    }
}

void RecentStore::save() const {
    nxkit::ensureDataDirectory();

    nlohmann::json root = nlohmann::json::array();
    for (const auto& file : this->entries) {
        root.push_back({{"id", file.id},
                        {"name", file.name},
                        {"size", file.size},
                        {"modified", file.updatedAt},
                        {"thumb", file.thumb},
                        {"provider_id", file.providerId},
                        {"provider", file.provider},
                        {"parent", file.parentId}});
    }

    std::ofstream output(nxkit::dataPath(kFileName), std::ios::trunc);
    if (!output.is_open()) return;
    output << root.dump(2);
}

}  // namespace cloud
