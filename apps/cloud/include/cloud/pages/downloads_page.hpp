//
// cloud: download task manager page.
//

#pragma once

#include <borealis.hpp>
#include <borealis/core/task.hpp>

#include <cstdint>
#include <functional>
#include <string>
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
        /// Large percent for active tasks, state text for finished ones.
        brls::Label* headline = nullptr;
        brls::Label* detail = nullptr;
        /// Progress track; null for compact finished rows.
        brls::View* bar = nullptr;
        DownloadState state = DownloadState::Queued;
        uint64_t lastBytes = 0;
        uint64_t lastMs = 0;
        /// Smoothed transfer rate in bytes per second.
        double speed = 0.0;
    };

    brls::Box* buildSelectBar();
    void rebuild();
    void buildEmptyState();
    brls::Box* sectionHeader(const std::string& title, size_t count,
                             bool spaced, brls::Label** summaryOut = nullptr);
    brls::Box* buildActiveCard(const DownloadTask& task, uint64_t now);
    brls::Box* buildFinishedRow(const DownloadTask& task, uint64_t now);
    void updateRow(Row& row, const DownloadTask& task, uint64_t nowMs);
    void updateSummary();

    const DownloadTask* findTask(uint64_t id) const;
    bool isSelected(uint64_t id) const;
    void toggleSelect(uint64_t id);
    void toggleSelectMode();
    void runRowAction(uint64_t id);
    void confirmDeleteSelected();
    void confirmClearFinished();
    void showDetails(uint64_t id);
    void showConfirm(const std::string& title, const std::string& message,
                     const std::string& confirmText, bool danger,
                     std::function<void()> onConfirm);
    /// Sidebar item of the active tab, used to jump back up from the list.
    brls::View* activeTabItem();
    /// Points the list's escape routes at the tab bar. Re-applied on every
    /// refresh because cards are recreated and the page may not be parented
    /// when it is first built.
    void applyTabRoutes();

    brls::Box* listBox = nullptr;
    brls::Box* selectBar = nullptr;
    brls::Label* selectText = nullptr;
    /// Right-aligned summary inside the "active" section header.
    brls::Label* activeSummary = nullptr;
    std::vector<Row> rows;
    std::vector<DownloadTask> latest;
    std::vector<uint64_t> selected;
    bool selecting = false;
    /// False until the first rebuild; lets the initial refresh build the
    /// empty state even though there is nothing to compare against.
    bool built = false;
    brls::RepeatingTask* timer = nullptr;
};

}  // namespace cloud
