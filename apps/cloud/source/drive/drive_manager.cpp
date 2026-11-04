//
// cloud: registry of available drive providers.
//

#include "cloud/drive/drive_manager.hpp"

#include "cloud/drive/quark_drive.hpp"

namespace cloud {

DriveManager& DriveManager::instance() {
    static DriveManager manager;
    return manager;
}

void DriveManager::registerProvider(std::shared_ptr<DriveProvider> provider) {
    providerList.push_back(std::move(provider));
}

void DriveManager::setCurrent(const std::string& id) {
    for (auto& provider : providerList) {
        if (provider->id() == id) {
            active = provider;
            return;
        }
    }
}

void DriveManager::init() {
    registerProvider(std::make_shared<QuarkDrive>());
    for (auto& provider : providerList) provider->loadCredentials();
    if (!providerList.empty()) active = providerList.front();
}

}  // namespace cloud
