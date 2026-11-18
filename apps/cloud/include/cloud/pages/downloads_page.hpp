//
// cloud: download task manager page.
//

#pragma once

#include <borealis.hpp>
#include <borealis/core/task.hpp>

#include <cstdint>
#include <memory>
#include <vector>

#include "cloud/drive/download_manager.hpp"

namespace cloud {

class DownloadPage : public brls::Box {
public:
    DownloadPage();
    ~DownloadPage() override;

    /// Called by the refresh timer; also used right after construction.
    void refresh();

private:
    struct Row {
        uint64_t id = 0;
        brls::Box* card = nullptr;
        brls::Label* check = nullptr;
        brls::Label* name = nullptr;
        brls::Label* status = nullptr;
        brls::Label* detail = nullptr;
        brls::View* bar = nullptr;
        DownloadState state = DownloadState::Queued;
        uint64_t lastBytes = 0;
        uint64_t lastMs = 0;
    };

    void rebuild();
    void updateRow(Row& row, const DownloadTask& task, uint64_t nowMs);
    void updateSummary();

    const DownloadTask* findTask(uint64_t id) const;
    bool isSelected(uint64_t id) const;
    void toggleSelect(uint64_t id);
    void toggleSelectMode();
    void runRowAction(uint64_t id);
    void confirmCancel(uint64_t id);
    void confirmDeleteSelected();
    void confirmClearFinished();
    void showDetails(uint64_t id);
    /// Sidebar item of the active tab, used to jump back up from the list.
    brls::View* activeTabItem();
    /// Points the list's escape routes at the tab bar. Re-applied on every
    /// refresh because cards are recreated and the page may not be parented
    /// when it is first built.
    void applyTabRoutes();

    brls::Box* listBox = nullptr;
    brls::Label* summaryLabel = nullptr;
    std::vector<Row> rows;
    std::vector<DownloadTask> latest;
    std::vector<uint64_t> selected;
    bool selecting = false;
    brls::RepeatingTask* timer = nullptr;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

}  // namespace cloud
