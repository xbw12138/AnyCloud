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
    item->setTabStyle(AutoTabBarStyle::ACCENT);
    item->setLabel(label);
    tabs->addTab(item, std::move(creator));
}

/// Placeholder used by the sections that have no backend support yet.
static brls::View* createSoonPage() {
    brls::Box* page = new brls::Box(brls::Axis::COLUMN);
    page->setGrow(1);
    // Same padding as the FileListPage header so the sections line up.
    page->setPadding(14, 20, 12, 20);

    CloudEmptyView* empty = new CloudEmptyView();
    empty->setContent(CloudEmptyView::Mode::Empty, "cloud/soon/title"_i18n,
                      "cloud/soon/desc"_i18n);
    empty->setAction("", nullptr);
    page->addView(empty);

    return page;
}

brls::View* MainActivity::createContentView() {
    AutoTabFrame* tabs = new AutoTabFrame();
    tabs->setSideBarPosition(AutoTabBarPosition::LEFT);
    tabs->setHorizontalMode(false);
    // Keep landing on the file browser when the app opens.
    tabs->setDefaultTabIndex(1);
    // The default sidebar width is too narrow for four-character labels.
    tabs->getSidebar()->setWidth(190);

    addTab(tabs, "cloud/tab/recent"_i18n, [] {
        return new FileListPage(FileListPage::Mode::Recent);
    });
    addTab(tabs, "cloud/tab/files"_i18n, [] { return new FileListPage(); });
    addTab(tabs, "cloud/tab/shared"_i18n, [] { return createSoonPage(); });
    addTab(tabs, "cloud/tab/favorites"_i18n, [] { return createSoonPage(); });
    addTab(tabs, "cloud/tab/profile"_i18n, [] { return new ProfilePage(); });

    tabs->setFontSize(22);

    return tabs;
}

}  // namespace cloud
