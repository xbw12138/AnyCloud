//
// cloud: main activity with the file browser and profile tabs.
//

#include "cloud/main_activity.hpp"

#include "cloud/pages/file_list_page.hpp"
#include "cloud/pages/profile_page.hpp"
#include "cloud/ui/cloud_empty_view.hpp"
#include "nxkit/ui/auto_tab_frame.hpp"

using namespace brls::literals;

namespace cloud {

static void addTab(AutoTabFrame* tabs, const std::string& label,
                   TabViewCreator creator) {
    AutoSidebarItem* item = new AutoSidebarItem();
    // Plain items take the active background colour, which turns the focused
    // tab into the rounded pill used by the Quark clients.
    item->setTabStyle(AutoTabBarStyle::PLAIN);
    item->setLabel(label);
    item->setHeight(46);
    item->setCornerRadius(23);
    item->setHighlightCornerRadius(23);
    item->setPadding(0, 24, 0, 24);
    item->setMargins(0, 4, 0, 4);
    tabs->addTab(item, std::move(creator));
}

/// Placeholder used by the sections that have no backend support yet.
static brls::View* createSoonPage() {
    brls::Box* page = new brls::Box(brls::Axis::COLUMN);
    page->setGrow(1);
    // Same padding as the FileListPage header so the sections line up.
    page->setPadding(20, 40, 12, 40);

    CloudEmptyView* empty = new CloudEmptyView();
    empty->setContent(CloudEmptyView::Mode::Empty, "cloud/soon/title"_i18n,
                      "cloud/soon/desc"_i18n);
    empty->setAction("", nullptr);
    page->addView(empty);

    return page;
}

brls::View* MainActivity::createContentView() {
    brls::Theme theme = brls::Application::getTheme();

    AutoTabFrame* tabs = new AutoTabFrame();
    tabs->setSideBarPosition(AutoTabBarPosition::TOP);
    // Keep landing on the file browser when the app opens.
    tabs->setDefaultTabIndex(1);
    tabs->setFontSize(19);
    tabs->setBackgroundColor(theme["cloud/bg"]);

    // Flat tab bar: no panel behind the pills, generous side padding so the
    // tabs line up with the page content below.
    brls::Box* bar = tabs->getSidebar();
    bar->setBackgroundColor(theme["cloud/bg"]);
    bar->setHeight(88);
    bar->setPadding(26, 40, 8, 40);

    tabs->setItemActiveBackgroundColor(theme["brls/accent"]);
    tabs->setItemActiveTextColor(nvgRGB(255, 255, 255));
    tabs->setItemDefaultBackgroundColor(nvgRGBA(0, 0, 0, 0));

    addTab(tabs, "cloud/tab/recent"_i18n, [] {
        return new FileListPage(FileListPage::Mode::Recent);
    });
    addTab(tabs, "cloud/tab/files"_i18n, [] { return new FileListPage(); });
    addTab(tabs, "cloud/tab/shared"_i18n, [] { return createSoonPage(); });
    addTab(tabs, "cloud/tab/favorites"_i18n, [] { return createSoonPage(); });
    addTab(tabs, "cloud/tab/profile"_i18n, [] { return new ProfilePage(); });

    return tabs;
}

}  // namespace cloud
