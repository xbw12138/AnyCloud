//
// cloud: background file downloads to the local filesystem.
//

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cloud/drive/drive_provider.hpp"

namespace cloud {

enum class DownloadState { Queued, Running, Done, Failed, Cancelled };

struct DownloadTask {
    uint64_t id = 0;
    std::string name;
    /// Display name of the drive the file came from, e.g. "115网盘".
    std::string provider;
    std::string path;
    uint64_t received = 0;
    uint64_t total = 0;
    DownloadState state = DownloadState::Queued;
    std::string error;
    /// False for records restored from disk: their download url is gone, so
    /// they can only be deleted, not retried.
    bool retryable = true;
};

class DownloadManager {
public:
    static DownloadManager& instance();

    /// Queues `file` into `directory` (created when missing).
    uint64_t start(std::shared_ptr<DriveProvider> provider, const DriveFile& file,
                   const std::string& directory);

    void cancel(uint64_t id);
    /// Re-queues a failed/cancelled task, resolving a fresh download url.
    void retry(uint64_t id);
    /// Drops a task from the list, cancelling it first when still running.
    void remove(uint64_t id);
    /// Drops finished (done/failed/cancelled) tasks from the list.
    void clearFinished();
    std::vector<DownloadTask> snapshot() const;

private:
    DownloadManager();

    struct TaskState {
        uint64_t id = 0;
        std::string name;
        std::string provider;
        std::string directory;
        std::string path;
        uint64_t received = 0;
        uint64_t total = 0;
        DownloadState state = DownloadState::Queued;
        std::string error;
        std::atomic<bool> cancelled{false};
        // Kept so a failed task can be retried later.
        std::shared_ptr<DriveProvider> providerRef;
        DriveFile file;
    };

    void launch(const std::shared_ptr<TaskState>& state);

    void update(const std::shared_ptr<TaskState>& state,
                std::function<void(TaskState&)> mutate);

    void loadHistory();
    void saveHistory();

    mutable std::mutex mutex;
    std::vector<std::shared_ptr<TaskState>> tasks;
    uint64_t nextId = 1;
};

}  // namespace cloud
