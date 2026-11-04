//
// cloud: entry point of the cloud drive app.
//

#include <borealis.hpp>

#include <cstdlib>

#include "cloud/drive/drive_manager.hpp"
#include "cloud/main_activity.hpp"
#include "cloud/pages/login_page.hpp"
#include "nxkit/app.hpp"

#ifdef NXKIT_WITH_MPV
#include "nxkit/media/mpv_core.hpp"
#endif

int main(int argc, char** argv) {
    // Same console defaults wiliwili uses: hardware decoding on, plus a small
    // demuxer cache so large streams do not stutter or fail to decode.
#ifdef NXKIT_WITH_MPV
    nxkit::MPVCore::HARDWARE_DEC = true;
    nxkit::MPVCore::PLAYER_HWDEC_METHOD = "auto";
    nxkit::MPVCore::INMEMORY_CACHE = 20;
#endif

    nxkit::AppConfig config;
    config.title = "夸克网盘";
    // Quark blue used by the pill tabs and focus rings.
    config.lightAccent = nvgRGB(30, 110, 240);
    config.darkAccent = nvgRGB(64, 140, 255);

    // Temporary: CLOUD_DEBUG_LOGIN=1 opens the QR login page directly.
    const bool debugLogin = std::getenv("CLOUD_DEBUG_LOGIN") != nullptr;

    // Created after runApp() has loaded the config, so providers see the
    // credentials saved by a previous session.
    config.createActivity = [debugLogin] -> brls::Activity* {
        cloud::DriveManager::instance().init();
        if (debugLogin) return new cloud::LoginPage();
        return new cloud::MainActivity();
    };

    return nxkit::runApp(config);
}
