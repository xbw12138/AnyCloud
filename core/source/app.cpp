//
// GF: application bootstrap shared by every nxkit app.
//

#include "nxkit/app.hpp"

#include <cpr/async_wrapper.h>
#include <curl/curl.h>

#include <chrono>
#include <cstdlib>

#include <borealis/views/applet_frame.hpp>

#include "nxkit/config.hpp"
#include "nxkit/paths.hpp"
#include "nxkit/ui/auto_tab_frame.hpp"
#include "nxkit/ui/cover_grid_page.hpp"
#include "nxkit/ui/qr_login_view.hpp"
#include "nxkit/ui/recycling_grid.hpp"
#include "nxkit/ui/settings_page.hpp"
#include "nxkit/ui/svg_image.hpp"
#include "nxkit/ui/text_box.hpp"
#include "nxkit/utils/thread_helper.hpp"

namespace nxkit {

static void registerFrameworkViews() {
    brls::Application::registerXMLView("SVGImage", SVGImage::create);
    brls::Application::registerXMLView("TextBox", TextBox::create);
    brls::Application::registerXMLView("AutoTabFrame", AutoTabFrame::create);
    brls::Application::registerXMLView("RecyclingGrid", RecyclingGrid::create);
    brls::Application::registerXMLView("CoverGridPage", CoverGridPage::create);
    brls::Application::registerXMLView("SettingsPage", SettingsPage::create);
    brls::Application::registerXMLView("QRLoginView", QRLoginView::create);
}

static void registerFrameworkTheme(const AppConfig& config) {
    brls::Theme& light = brls::Theme::getLightTheme();
    brls::Theme& dark = brls::Theme::getDarkTheme();

    light.addColor("brls/accent", config.lightAccent);
    dark.addColor("brls/accent", config.darkAccent);

    // Card / skeleton colors used by CoverCard and RecyclingGrid.
    light.addColor("color/grey_1", nvgRGB(245, 246, 247));
    dark.addColor("color/grey_1", nvgRGB(51, 52, 53));

    light.addColor("color/grey_3", nvgRGBA(200, 200, 200, 16));
    dark.addColor("color/grey_3", nvgRGBA(160, 160, 160, 160));

    // Grey text readable in both themes.
    light.addColor("font/grey", nvgRGB(148, 153, 160));
    dark.addColor("font/grey", nvgRGB(148, 153, 160));

    // Flat, high contrast palette used by the cloud pages: a single dark
    // canvas with slightly lighter cards and no visible borders.
    light.addColor("cloud/bg", nvgRGB(247, 248, 250));
    dark.addColor("cloud/bg", nvgRGB(20, 21, 25));

    light.addColor("cloud/surface", nvgRGB(255, 255, 255));
    dark.addColor("cloud/surface", nvgRGB(30, 32, 37));

    light.addColor("cloud/surface_alt", nvgRGB(240, 242, 245));
    dark.addColor("cloud/surface_alt", nvgRGB(40, 43, 49));

    light.addColor("cloud/border", nvgRGBA(0, 0, 0, 20));
    dark.addColor("cloud/border", nvgRGBA(255, 255, 255, 26));

    light.addColor("cloud/text", nvgRGB(28, 30, 34));
    dark.addColor("cloud/text", nvgRGB(236, 238, 242));

    light.addColor("cloud/text_secondary", nvgRGB(132, 138, 148));
    dark.addColor("cloud/text_secondary", nvgRGB(140, 149, 162));
}

static brls::Activity* createFallbackActivity(const AppConfig& config) {
    brls::Label* label = new brls::Label();
    label->setText(config.title);
    label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    label->setVerticalAlign(brls::VerticalAlign::CENTER);
    label->setGrow(1);

    brls::Box* box = new brls::Box(brls::Axis::COLUMN);
    box->setGrow(1);
    box->addView(label);
    return new brls::Activity(box);
}

int runApp(const AppConfig& config) {
    brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);

    ensureDataDirectory();
    Config::instance().load(config.configPath);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    cpr::async::startup(THREAD_POOL_MIN_THREAD_NUM, THREAD_POOL_MAX_THREAD_NUM,
                        std::chrono::milliseconds(5000));

    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init Borealis application");
        cpr::async::cleanup();
        curl_global_cleanup();
        return EXIT_FAILURE;
    }

    brls::Application::createWindow(config.title);
    brls::Application::setGlobalQuit(config.globalQuit);

    registerFrameworkTheme(config);
    registerFrameworkViews();
    if (config.registerViews) config.registerViews();

    brls::Application::pushActivity(
        config.createActivity ? config.createActivity()
                              : createFallbackActivity(config));

    while (brls::Application::mainLoop()) {
    }

    cpr::async::cleanup();
    curl_global_cleanup();
    return EXIT_SUCCESS;
}

}  // namespace nxkit
