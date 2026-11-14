//
// cloud: main activity with the file browser and profile tabs.
//

#include "cloud/main_activity.hpp"

#include "cloud/pages/file_list_page.hpp"
#include "cloud/pages/profile_page.hpp"
#include "nxkit/ui/auto_tab_frame.hpp"

using namespace brls::literals;

namespace cloud {

static void addTab(AutoTabFrame* tabs, const std::string& label,
                   TabViewCreator creator) {
    AutoSidebarItem* item = new AutoSidebarItem();
    // Plain items take the active background colour, so the current section is
    // a single accent pill instead of a heavy tab bar.
    item->setTabStyle(AutoTabBarStyle::PLAIN);
    item->setLabel(label);
    item->setHeight(40);
    item->setCornerRadius(20);
    item->setHighlightCornerRadius(20);
    item->setPadding(0, 22, 0, 22);
    item->setMargins(0, 3, 0, 3);
    tabs->addTab(item, std::move(creator));
}

brls::View* MainActivity::createContentView() {
    brls::Theme theme = brls::Application::getTheme();

    AutoTabFrame* tabs = new AutoTabFrame();
    // Minimal top navigation: the home screen owns the rest of the display.
    tabs->setSideBarPosition(AutoTabBarPosition::TOP);
    // Land on the recent page (TV home) when the app opens.
    tabs->setDefaultTabIndex(0);
    tabs->setFontSize(17);
    tabs->setBackgroundColor(theme["cloud/bg"]);

    brls::Box* bar = tabs->getSidebar();
    bar->setBackgroundColor(theme["cloud/bg"]);
    bar->setHeight(72);
    bar->setPadding(18, 56, 4, 56);
    bar->setLineColor(theme["cloud/border"]);
    bar->setLineBottom(1);

    tabs->setItemActiveBackgroundColor(theme["brls/accent"]);
    tabs->setItemActiveTextColor(nvgRGB(255, 255, 255));
    tabs->setItemDefaultBackgroundColor(nvgRGBA(0, 0, 0, 0));

    addTab(tabs, "cloud/tab/recent"_i18n, [] {
        return new FileListPage(FileListPage::Mode::Recent);
    });
    addTab(tabs, "cloud/tab/files"_i18n, [] { return new FileListPage(); });
    addTab(tabs, "cloud/tab/profile"_i18n, [] { return new ProfilePage(); });

    // L / R switch sections from anywhere. The top bar is still reachable by
    // moving up out of the content, but this keeps section switching working
    // even when a page decides to keep the focus inside its own content.
    tabs->registerAction("cloud/tab/prev"_i18n, brls::BUTTON_LB,
                         [tabs](brls::View*) {
                             tabs->focus2LastTab();
                             return true;
                         });
    tabs->registerAction("cloud/tab/next"_i18n, brls::BUTTON_RB,
                         [tabs](brls::View*) {
                             tabs->focus2NextTab();
                             return true;
                         });

    return tabs;
}

}  // namespace cloud
