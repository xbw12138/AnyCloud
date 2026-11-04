//
// cloud: file browser page (list, navigate, file operations).
//

#pragma once

#include <borealis.hpp>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cloud/drive/drive_provider.hpp"
#include "nxkit/ui/auto_tab_frame.hpp"

class RecyclingGrid;
class RecyclingGridDataSource;

namespace cloud {

class CloudEmptyView;

/// Browses a provider's directories. In picker mode only folders are shown
/// and Y picks the current directory (used by "move to..."). Recent mode
/// lists media opened from the browser, persisted by RecentStore.
class FileListPage : public brls::Box, public BackRequestHandler {
public:
    enum class Mode { Browse, Picker, Recent };

    /// Browse mode: lists the provider root.
    FileListPage();
    /// Lists the locally persisted recent views.
    explicit FileListPage(Mode mode);
    /// Picker mode: pick a directory, callback fires with its id.
    FileListPage(std::function<void(const std::string& dirId,
                                    const std::string& dirName)> onPick);
    ~FileListPage() override;

    void willAppear(bool resetState = false) override;
    brls::View* getDefaultFocus() override;

    /// Opens the item at `index` (called by the data source).
    void openItem(size_t index);
    /// Shows the actions menu for the item at `index` (X button).
    void showItemActions(size_t index);

    /// True when the grid is showing tall single-column rows.
    bool isListMode() const { return this->listMode; }

    /// Consumes B while inside a sub folder; at the root it falls through to
    /// the tab frame, which moves focus back to the sidebar.
    bool onBackRequested() override;

private:
    enum class SortMode { Time, Name };
    void setup();
    void enterFolder(const std::string& dirId, const std::string& name);
    bool goUp();
    void refresh();
    void loadNextPage();
    void applyFiles(const std::vector<DriveFile>& files, int total,
                    bool firstPage, const std::string& error);
    void refreshRecent();
    void applySort();
    void showSortMenu();
    void toggleViewMode();
    void updateBreadcrumb();
    void updateSubtitle();

    /// Replaces the list with one of the empty states.
    void showLoginState();
    void showEmptyState();
    void showErrorState(const std::string& error);
    /// Shows the list again and restores focus if needed.
    void showGrid();
    /// True when `view` is the given ancestor or one of its descendants.
    static bool isFocusWithin(brls::View* view, brls::View* ancestor);

    void promptMakeDir();
    void pickCurrentDirectory();
    void promptRename(const DriveFile& file);
    void confirmDelete(const DriveFile& file);
    void pickMoveTarget(const DriveFile& file);

    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    std::shared_ptr<DriveProvider> provider;
    Mode mode = Mode::Browse;
    std::function<void(const std::string&, const std::string&)> pickCallback;

    /// Path from the root: (dirId, dirName). Root is ("0", "").
    std::vector<std::pair<std::string, std::string>> pathStack;

    RecyclingGrid* grid = nullptr;
    RecyclingGridDataSource* dataSource = nullptr;
    CloudEmptyView* emptyView = nullptr;
    brls::Label* breadcrumb = nullptr;
    brls::Label* pathLabel = nullptr;
    brls::Label* subtitleLabel = nullptr;
    brls::Label* hintLabel = nullptr;
    brls::Button* viewToggle = nullptr;

    bool listMode = false;
    SortMode sortMode = SortMode::Time;

    int page = 0;
    int total = -1;
    bool loading = false;
    /// Login state at the last refresh, used to reload after signing in.
    bool wasLoggedIn = false;
    /// Incremented on every refresh so in-flight responses can be discarded.
    uint64_t requestSeq = 0;
};

}  // namespace cloud
