//
// cloud: account card + provider actions page.
//

#pragma once

#include <borealis.hpp>

#include <functional>
#include <memory>
#include <string>

#include "cloud/drive/drive_provider.hpp"

namespace cloud {

class StorageBar;
class ActionTile;
class AvatarView;

/// Lets the user pick another drive. Shared by the account page and the badge
/// in the tab bar; does nothing when only one drive is available.
void showProviderPicker(std::function<void()> onChanged = {});

/// Account overview: avatar, nickname, membership and storage usage inside a
/// single card, with refresh / sign-in actions underneath.
class ProfilePage : public brls::Box {
public:
    ProfilePage();
    ~ProfilePage() override { *this->alive = false; }

    void willAppear(bool resetState = false) override;

    /// Drops cached account data and reloads it, e.g. after the drive was
    /// changed from the badge in the tab bar.
    void reload();

private:
    void build();
    void refreshAccount();
    void applyAccount();
    void loadAvatar(const std::string& url);

    void openLogin();
    void logout();
    void openProviderMenu();
    void installDesktopIcon();
    void checkUpdate();

    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    brls::Label* avatarLabel = nullptr;
    AvatarView* avatarView = nullptr;
    std::string avatarUrl;
    brls::Label* nicknameLabel = nullptr;
    brls::Label* memberLabel = nullptr;
    brls::Box* capacityBox = nullptr;
    brls::Label* capacityValue = nullptr;
    StorageBar* capacityBar = nullptr;

    ActionTile* refreshTile = nullptr;
    brls::Box* switchButton = nullptr;
    ActionTile* accountTile = nullptr;
    ActionTile* desktopTile = nullptr;
    ActionTile* updateTile = nullptr;

    bool loggedIn = false;
    bool refreshing = false;
    bool checkingUpdate = false;
    /// Bumped per account request so late replies and the watchdog can be told
    /// apart from the current one.
    uint64_t accountRequest = 0;
    AccountInfo account;
};

}  // namespace cloud
