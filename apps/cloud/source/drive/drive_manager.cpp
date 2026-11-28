//
// cloud: registry of available drive providers.
//

#include "cloud/drive/drive_manager.hpp"

#include "cloud/drive/aliyun_drive.hpp"
#include "cloud/drive/baidu_drive.hpp"
#include "cloud/drive/drive115.hpp"
#include "cloud/drive/quark_drive.hpp"
#include "cloud/drive/smb_drive.hpp"
#include "nxkit/auth_store.hpp"

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
            switchGeneration++;
            nxkit::AuthStore::instance().set("cloud.active_provider", id);
            return;
        }
    }
}

std::shared_ptr<DriveProvider> DriveManager::find(const std::string& id) const {
    if (id.empty()) return nullptr;
    for (const auto& provider : providerList)
        if (provider->id() == id) return provider;
    return nullptr;
}

std::shared_ptr<DriveProvider> DriveManager::findByNameOrId(
    const std::string& key) const {
    if (key.empty()) return nullptr;
    for (const auto& provider : providerList)
        if (provider->id() == key || provider->name() == key) return provider;
    return nullptr;
}

void DriveManager::init() {
    registerProvider(std::make_shared<QuarkDrive>());
    registerProvider(std::make_shared<AliyunDrive>());
    registerProvider(std::make_shared<Drive115>());
    registerProvider(std::make_shared<BaiduDrive>());
    registerProvider(std::make_shared<SmbDrive>());
    for (auto& provider : providerList) provider->loadCredentials();

    // Keep the drive the user picked last time.
    const std::string saved =
        nxkit::AuthStore::instance().get("cloud.active_provider");
    if (!saved.empty()) setCurrent(saved);
    if (!active && !providerList.empty()) active = providerList.front();
}

}  // namespace cloud
