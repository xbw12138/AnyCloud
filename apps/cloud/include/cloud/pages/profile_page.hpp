//
// cloud: account card + provider actions page.
//

#pragma once

#include <borealis.hpp>

#include <memory>
#include <string>

#include "cloud/drive/drive_provider.hpp"

namespace cloud {

class StorageBar;
class ActionTile;

/// Account overview: avatar, nickname, membership and storage usage inside a
/// single card, with refresh / sign-in actions underneath.
class ProfilePage : public brls::Box {
public:
    ProfilePage();
    ~ProfilePage() override { *this->alive = false; }

    void willAppear(bool resetState = false) override;

private:
    void build();
    void refreshAccount();
    void applyAccount();

    void openLogin();
    void logout();
    void openProviderMenu();
    void installDesktopIcon();

    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    brls::Label* providerLabel = nullptr;
    brls::Label* avatarLabel = nullptr;
    brls::Label* nicknameLabel = nullptr;
    brls::Label* memberLabel = nullptr;
    brls::Box* capacityBox = nullptr;
    brls::Label* capacityValue = nullptr;
    StorageBar* capacityBar = nullptr;

    ActionTile* refreshTile = nullptr;
    ActionTile* accountTile = nullptr;
    ActionTile* providerTile = nullptr;
    ActionTile* desktopTile = nullptr;

    bool loggedIn = false;
    bool refreshing = false;
    AccountInfo account;
};

}  // namespace cloud
