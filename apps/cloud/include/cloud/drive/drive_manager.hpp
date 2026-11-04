//
// cloud: registry of available drive providers.
//

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cloud/drive/drive_provider.hpp"

namespace cloud {

/// Owns every known provider and tracks the active one.
class DriveManager {
public:
    static DriveManager& instance();

    void registerProvider(std::shared_ptr<DriveProvider> provider);
    std::vector<std::shared_ptr<DriveProvider>>& providers() { return providerList; }

    /// The active provider, never null after init() (defaults to Quark).
    std::shared_ptr<DriveProvider> current() const { return active; }
    void setCurrent(const std::string& id);

    /// Registers built-in providers and loads their credentials.
    void init();

private:
    DriveManager() = default;

    std::vector<std::shared_ptr<DriveProvider>> providerList;
    std::shared_ptr<DriveProvider> active;
};

}  // namespace cloud
