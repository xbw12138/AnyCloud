//
// cloud: local http bridge that exposes SMB files to mpv and the image/pdf
// viewers, which only speak http.
//

#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "cloud/drive/smb_session.hpp"

namespace cloud {

/// Serves the active SMB share on 127.0.0.1 so every player/viewer that talks
/// http (mpv, libcurl based downloader, image and document viewers) can read
/// the file with plain Range requests.
class SmbBridge {
public:
    static SmbBridge& instance();

    /// Replaces the credentials used by incoming requests.
    void setConfig(const SmbConfig& config);
    SmbConfig config() const;

    /// Playable url for a path inside the share. Empty when the server could
    /// not be started.
    std::string urlFor(const std::string& path);

    /// Starts the listener if needed and returns its base url (empty when it
    /// could not be started). Cheap after the first call, so the share form
    /// can run it once and keep media opens free of any wait.
    std::string start();

private:
    SmbBridge() = default;
    ~SmbBridge() = default;

    /// Accepts clients and hands them to the workers. Runs on its own thread.
    void run();
    /// Serves queued sockets one at a time. Runs on its own thread.
    void work();

    static void* listenEntry(void* arg);
    static void* workEntry(void* arg);

    mutable std::mutex mutex;
    std::condition_variable ready;
    SmbConfig credentials;
    std::string base;
    bool starting = false;
    bool finished = false;

    std::mutex queueMutex;
    std::condition_variable queueReady;
    std::vector<int> pending;
};

}  // namespace cloud
