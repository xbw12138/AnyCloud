//
// cloud: entry point of the cloud drive app.
//

#include <borealis.hpp>

#include <cstdlib>
#include <fstream>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "cloud/drive/drive_manager.hpp"
#include "cloud/main_activity.hpp"
#include "cloud/pages/login_page.hpp"
#include "nxkit/app.hpp"

#ifdef NXKIT_WITH_MPV
#include "nxkit/media/mpv_core.hpp"
#endif

#ifndef NXKIT_APP_VERSION
#define NXKIT_APP_VERSION "unknown"
#endif

#ifdef __SWITCH__
// 把当前版本落盘，供应用商店（updater.nro）按版本号判断是否可更新；
// 手动用 USB 推送、没经过商店安装的 NRO 没有 .version 文件，商店只能退回
// 按文件体积比较，体积相同的两个版本就会被误判为「已是最新」。
static void recordInstalledVersion() {
    std::ofstream file("sdmc:/switch/cloud.nro.version",
                       std::ios::binary | std::ios::trunc);
    if (file) file << NXKIT_APP_VERSION;
}
#else
static void recordInstalledVersion() {}
#endif

#ifdef BUILTIN_NSP
// 1.6.6 起桌面图标改用新的转发器标题号；以新图标启动时删掉旧标题，
// 避免主菜单同时留着旧的快捷方式。以旧标题运行时跳过，避免删除正在运行的标题。
// 注意：这里不能用 mini::GetTitleID()，它读取的是 InstallSD 内部记录的列表，
// 启动时列表为空，越界访问会直接崩溃。
static void removeLegacyDesktopIcon() {
    constexpr u64 kLegacyTitleId = 0x010FF000FFFF0103ULL;
    u64 programId = 0;
    if (R_FAILED(svcGetInfo(&programId, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0)))
        return;
    if (programId == kLegacyTitleId) return;
    if (R_FAILED(nsInitialize())) return;
    nsDeleteApplicationCompletely(kLegacyTitleId);
    nsExit();
}
#else
static void removeLegacyDesktopIcon() {}
#endif

int main(int argc, char** argv) {
    recordInstalledVersion();
    removeLegacyDesktopIcon();
    // Same console defaults wiliwili uses: hardware decoding on, plus a small
    // demuxer cache so large streams do not stutter or fail to decode.
#ifdef NXKIT_WITH_MPV
    nxkit::MPVCore::HARDWARE_DEC = true;
    nxkit::MPVCore::PLAYER_HWDEC_METHOD = "auto";
    nxkit::MPVCore::INMEMORY_CACHE = 20;
#endif

    nxkit::AppConfig config;
    config.title = "AnyCloud";
    // Teal accent matching the AnyCloud icon.
    config.lightAccent = nvgRGB(20, 190, 140);
    config.darkAccent = nvgRGB(60, 220, 170);

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
