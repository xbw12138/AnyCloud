//
// cloud: account + provider settings page.
//

#pragma once

#include <memory>
#include <string>

#include "cloud/drive/drive_provider.hpp"
#include "nxkit/ui/settings_page.hpp"

namespace cloud {

class ProfilePage : public nxkit::SettingsPage {
public:
    ProfilePage();
    ~ProfilePage() override { *this->alive = false; }

    void willAppear(bool resetState) override;

private:
    void rebuild();
    void refreshAccount();

    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    bool loggedIn = false;
    AccountInfo account;
};

}  // namespace cloud
