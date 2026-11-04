//
// cloud: account + provider settings page.
//

#include "cloud/pages/profile_page.hpp"

#include "cloud/drive/drive_manager.hpp"
#include "cloud/pages/login_page.hpp"

using namespace brls::literals;

namespace cloud {

static std::string formatCapacity(uint64_t bytes) {
    double gb = (double)bytes / (1024.0 * 1024.0 * 1024.0);
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f GB", gb);
    return buf;
}

ProfilePage::ProfilePage() {
    this->rebuild();
    this->refreshAccount();
}

void ProfilePage::willAppear(bool resetState) {
    nxkit::SettingsPage::willAppear(resetState);
    this->refreshAccount();
}

void ProfilePage::refreshAccount() {
    auto provider = DriveManager::instance().current();
    this->loggedIn = provider->isLoggedIn();
    if (!this->loggedIn) {
        brls::sync([this, alive = this->alive] {
            if (!*alive) return;
            this->rebuild();
        });
        return;
    }
    provider->accountInfo(
        [this, alive = this->alive](const AccountInfo& info,
                                    const std::string& error) {
            brls::sync([this, alive, info, error] {
                if (!*alive) return;
                if (error.empty()) this->account = info;
                this->rebuild();
            });
        });
}

void ProfilePage::rebuild() {
    this->clear();

    auto& manager = DriveManager::instance();
    auto provider = manager.current();

    this->addSection("cloud/profile/section/provider"_i18n);
    if (manager.providers().size() > 1) {
        std::vector<std::string> names;
        int selected = 0;
        for (size_t i = 0; i < manager.providers().size(); i++) {
            names.push_back(manager.providers()[i]->name());
            if (manager.providers()[i]->id() == provider->id())
                selected = (int)i;
        }
        this->addSelector(
            "cloud/profile/provider"_i18n, names, selected, [](int index) {
                auto& providers = DriveManager::instance().providers();
                if (index >= 0 && index < (int)providers.size())
                    DriveManager::instance().setCurrent(providers[index]->id());
            });
    } else {
        this->addInfo("cloud/profile/provider"_i18n, provider->name());
    }

    this->addSection("cloud/profile/section/account"_i18n);
    if (this->loggedIn) {
        this->addInfo("cloud/profile/nickname"_i18n,
                      this->account.nickname.empty()
                          ? "cloud/profile/unknown"_i18n
                          : this->account.nickname);
        if (!this->account.memberType.empty())
            this->addInfo("cloud/profile/member"_i18n, this->account.memberType);
        if (this->account.totalCapacity > 0)
            this->addInfo("cloud/profile/capacity"_i18n,
                          formatCapacity(this->account.usedCapacity) + " / " +
                              formatCapacity(this->account.totalCapacity));
        this->addAction("cloud/profile/logout"_i18n, "", [this] {
            DriveManager::instance().current()->logout();
            this->loggedIn = false;
            this->account = AccountInfo();
            this->rebuild();
            brls::Application::notify("cloud/profile/logged_out"_i18n);
        });
    } else {
        this->addAction("cloud/profile/login"_i18n, "", [] {
            brls::Application::pushActivity(new LoginPage());
        });
    }

    this->addSection("cloud/profile/section/about"_i18n);
    this->addInfo("cloud/profile/version"_i18n, "1.0.0");
    this->addAction("cloud/profile/refresh"_i18n, "", [this] {
        this->refreshAccount();
        brls::Application::notify("cloud/profile/refreshing"_i18n);
    });

    this->reload();
}

}  // namespace cloud
