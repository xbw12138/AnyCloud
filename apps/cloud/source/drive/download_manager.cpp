//
// cloud: background file downloads to the local filesystem.
//

#include "cloud/drive/download_manager.hpp"

#include <cpr/async.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include <borealis/core/application.hpp>
#include <nlohmann/json.hpp>

#include "nxkit/http_client.hpp"
#include "nxkit/paths.hpp"

namespace cloud {

namespace {

const char* kHistoryFileName = "downloads.json";

}  // namespace

namespace {

std::string sanitizeName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c < 0x20) {
            out.push_back('_');
        } else if (c == '?' || c == '*' || c == '"' || c == '<' ||
                   c == '>' || c == '|') {
            // Characters FAT/exFAT reject; the SD card would fail to open.
            out.push_back('_');
        } else {
            out.push_back((char)c);
        }
    }
    // FAT also dislikes trailing dots and spaces.
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))
        out.pop_back();
    // Keep well under the 255-byte filesystem component limit.
    if (out.size() > 180) {
        out.resize(180);
        while (!out.empty() && ((unsigned char)out.back() & 0xC0) == 0x80)
            out.pop_back();
        if (!out.empty() && (unsigned char)out.back() >= 0xC0) out.pop_back();
    }
    if (out.empty() || out == "." || out == "..") out = "download";
    return out;
}

/// ASCII-only fallback used when FatFs refuses the original name. Non-ASCII
/// bytes collapse into a single underscore so the extension stays readable.
std::string asciiName(const std::string& name) {
    std::string out;
    bool lastUnderscore = false;
    for (unsigned char c : name) {
        bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                       c == '-';
        if (allowed) {
            out.push_back((char)c);
            lastUnderscore = false;
        } else if (!lastUnderscore) {
            out.push_back('_');
            lastUnderscore = true;
        }
    }
    if (out.empty() || out == "." || out == "..") out = "download";
    // A fully non-ASCII title collapses into "_.mp4"; give it a real stem.
    size_t dot = out.find_last_of('.');
    std::string stem = dot == std::string::npos ? out : out.substr(0, dot);
    if (stem.find_first_not_of("_-") == std::string::npos)
        out = "download" + (dot == std::string::npos ? "" : out.substr(dot));
    if (out.size() > 80) {
        size_t mark = out.find_last_of('.');
        std::string ext =
            (mark != std::string::npos && out.size() - mark <= 10)
                ? out.substr(mark)
                : std::string();
        out = out.substr(0, 80 - ext.size()) + ext;
    }
    return out;
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

bool pathExists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}

/// Size of a regular file, 0 when missing.
uint64_t fileSize(const std::string& path) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return 0;
    return (uint64_t)info.st_size;
}

bool isDirectory(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::string normalizeDirectory(const std::string& dir) {
    std::string out = dir;
    size_t first = out.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    out.erase(0, first);
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t' ||
                            out.back() == '\r' || out.back() == '\n'))
        out.pop_back();
#ifdef __SWITCH__
    if (out == "sdmc:") out = "sdmc:/";
#endif
    return out;
}

std::string uniquePath(const std::string& path) {
    if (!pathExists(path)) return path;
    size_t slash = path.find_last_of('/');
    std::string dir = slash == std::string::npos ? "" : path.substr(0, slash + 1);
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
    std::string ext = dot == std::string::npos ? "" : name.substr(dot);
    for (int i = 1; i < 10000; i++) {
        std::string candidate =
            dir + stem + " (" + std::to_string(i) + ")" + ext;
        if (!pathExists(candidate)) return candidate;
    }
    return path;
}

void makeDirs(const std::string& path) {
    if (path.empty()) return;
    std::string current;
    size_t start = 0;
    if (path[0] == '/') {
        current = "/";
        start = 1;
    }
    while (start < path.size()) {
        size_t slash = path.find('/', start);
        std::string part = path.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty()) {
            current = current.empty() ? part : joinPath(current, part);
            mkdir(current.c_str(), 0777);
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
}

cpr::Header parseHeaders(const std::string& block) {
    cpr::Header headers;
    size_t start = 0;
    while (start < block.size()) {
        size_t end = block.find('\n', start);
        std::string line = block.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            size_t first = value.find_first_not_of(' ');
            if (first != std::string::npos) value = value.substr(first);
            name.erase(0, name.find_first_not_of(' '));
            name.erase(name.find_last_not_of(' ') + 1);
            if (!name.empty()) headers[name] = value;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return headers;
}

}  // namespace

DownloadManager& DownloadManager::instance() {
    static DownloadManager manager;
    return manager;
}

DownloadManager::DownloadManager() { this->loadHistory(); }

void DownloadManager::loadHistory() {
    std::ifstream in(nxkit::dataPath(kHistoryFileName));
    if (!in) return;

    nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
    if (root.is_discarded() || !root.is_array()) return;

    std::lock_guard<std::mutex> lock(this->mutex);
    for (const auto& item : root) {
        if (!item.is_object()) continue;

        auto state = std::make_shared<TaskState>();
        state->name = item.value("name", std::string());
        state->provider = item.value("provider", std::string());
        state->directory = item.value("directory", std::string());
        state->path = item.value("path", std::string());
        state->error = item.value("error", std::string());
        if (state->name.empty()) continue;

        int raw = item.value("state", (int)DownloadState::Failed);
        if (raw < (int)DownloadState::Queued ||
            raw > (int)DownloadState::Cancelled)
            raw = (int)DownloadState::Failed;
        state->state = (DownloadState)raw;
        state->received = item.value("received", (uint64_t)0);
        state->total = item.value("total", (uint64_t)0);
        // Restored records have no provider/file handle, so they can only be
        // inspected and deleted.
        state->cancelled.store(true);
        state->id = this->nextId++;
        this->tasks.push_back(state);
    }
}

void DownloadManager::saveHistory() {
    constexpr size_t kMaxRecords = 200;

    nlohmann::json root = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        // Newest first while collecting, then flip back: keeps the file
        // bounded without keeping stale entries forever.
        size_t kept = 0;
        for (auto it = this->tasks.rbegin();
             it != this->tasks.rend() && kept < kMaxRecords; ++it, ++kept) {
            const auto& task = *it;
            nlohmann::json item;
            item["name"] = task->name;
            item["provider"] = task->provider;
            item["directory"] = task->directory;
            item["path"] = task->path;
            item["received"] = task->received;
            item["total"] = task->total;
            if (task->state == DownloadState::Queued ||
                task->state == DownloadState::Running) {
                // Cannot resume after a restart: remember it as interrupted.
                item["state"] = (int)DownloadState::Failed;
                item["error"] = "interrupted";
            } else {
                item["state"] = (int)task->state;
                item["error"] = task->error;
            }
            root.push_back(std::move(item));
        }
    }
    std::reverse(root.begin(), root.end());

    nxkit::ensureDataDirectory();
    std::ofstream out(nxkit::dataPath(kHistoryFileName), std::ios::trunc);
    if (!out) return;
    out << root.dump();
}

uint64_t DownloadManager::start(std::shared_ptr<DriveProvider> provider,
                                const DriveFile& file,
                                const std::string& directory) {
    auto state = std::make_shared<TaskState>();
    state->name = file.name;
    state->provider = provider ? provider->name() : std::string();
    state->directory = directory;
    state->providerRef = provider;
    state->file = file;
    {
        std::lock_guard<std::mutex> lock(mutex);
        state->id = nextId++;
        tasks.push_back(state);
    }

    this->launch(state);
    // Remember the task right away so an app exit mid-download still leaves a
    // visible history entry.
    this->saveHistory();
    return state->id;
}

void DownloadManager::launch(const std::shared_ptr<TaskState>& state) {
    auto provider = state->providerRef;
    if (!provider) {
        this->update(state, [](TaskState& task) {
            task.state = DownloadState::Failed;
            task.error = "missing drive provider";
        });
        return;
    }

    provider->downloadInfo(
        state->file.id,
        [state, provider, name = state->name,
         directory = state->directory](const std::string& url,
                                       const std::string& headers,
                                       const std::string& error) {
            // Cancelled while the provider was still resolving the url.
            if (state->cancelled.load()) {
                DownloadManager::instance().update(
                    state, [](TaskState& task) {
                        task.state = DownloadState::Cancelled;
                    });
                return;
            }
            if (!error.empty() || url.empty()) {
                DownloadManager::instance().update(
                    state, [&](TaskState& task) {
                        task.state = DownloadState::Failed;
                        task.error = error.empty() ? "empty download url" : error;
                    });
                return;
            }

            std::string dir = normalizeDirectory(directory);
            if (dir.empty()) {
                DownloadManager::instance().update(
                    state, [](TaskState& task) {
                        task.state = DownloadState::Failed;
                        task.error = "no download folder selected";
                    });
                return;
            }
            makeDirs(dir);
            if (!isDirectory(dir)) {
                DownloadManager::instance().update(
                    state, [&](TaskState& task) {
                        task.state = DownloadState::Failed;
                        task.error = "cannot access download folder: " + dir;
                    });
                return;
            }
            std::string path = uniquePath(joinPath(dir, sanitizeName(name)));
            {
                std::lock_guard<std::mutex> lock(
                    DownloadManager::instance().mutex);
                state->path = path;
                state->state = DownloadState::Running;
            }

            cpr::async([state, url, headers, path, dir, name] {
                std::string dest = path;
                FILE* file = fopen(dest.c_str(), "wb");
                if (!file) {
                    int openError = errno;
                    // FatFs on Switch reports ENOENT for names it cannot
                    // encode (non-ASCII, reserved chars). Probe the folder to
                    // tell a bad name apart from an unwritable directory.
                    std::string probePath = joinPath(dir, "cloud_probe.tmp");
                    FILE* probeFile = fopen(probePath.c_str(), "wb");
                    if (!probeFile) {
                        std::string reason = strerror(errno);
                        DownloadManager::instance().update(
                            state, [&](TaskState& task) {
                                task.state = DownloadState::Failed;
                                task.error = "cannot write to download folder: " +
                                             dir + " (" + reason + ")";
                            });
                        return;
                    }
                    fclose(probeFile);
                    unlink(probePath.c_str());

                    dest = uniquePath(joinPath(dir, asciiName(name)));
                    DownloadManager::instance().update(
                        state, [&](TaskState& task) { task.path = dest; });
                    file = fopen(dest.c_str(), "wb");
                    if (!file) {
                        std::string reason = strerror(openError);
                        DownloadManager::instance().update(
                            state, [&](TaskState& task) {
                                task.state = DownloadState::Failed;
                                task.error = "cannot open destination file: " +
                                             dest + " (" + reason + ")";
                            });
                        return;
                    }
                }

                cpr::Header baseHeaders = parseHeaders(headers);

                bool finished = false;
                std::string failure;

                if (!state->cancelled.load()) {
                    DownloadManager::instance().update(
                        state, [](TaskState& task) { task.received = 0; });

                    cpr::Session session;
                    session.SetUrl(cpr::Url{url});
                    session.SetOption(baseHeaders);
                    session.SetOption(cpr::ConnectTimeout{20000});
                    session.SetOption(cpr::Redirect{true});
                    nxkit::HttpClient::applyCaBundle(session);
#ifdef __SWITCH__
                    session.SetOption(cpr::VerifySsl{false});
#endif
                    session.SetWriteCallback(cpr::WriteCallback(
                        [file, state](std::string data, intptr_t) -> bool {
                            if (state->cancelled.load()) return false;
                            if (data.empty()) return true;
                            size_t written =
                                fwrite(data.data(), 1, data.size(), file);
                            return written == data.size();
                        }));
                    session.SetProgressCallback(cpr::ProgressCallback(
                        [state](cpr::cpr_pf_arg_t total, cpr::cpr_pf_arg_t now,
                                cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t,
                                intptr_t) -> bool {
                            // While the system menu is in front, stop reading so
                            // neither the network nor the SD card competes with
                            // the console's UI; the transfer continues once the
                            // app regains focus.
                            while (!brls::Application::isWindowFocused() &&
                                   !state->cancelled.load())
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds(50));
                            DownloadManager::instance().update(
                                state, [&](TaskState& task) {
                                    if (total > 0)
                                        task.total = (uint64_t)total;
                                    task.received = (uint64_t)now;
                                });
                            return !state->cancelled.load();
                        }));

                    cpr::Response response = session.Get();
                    fclose(file);

                    if (!response.error.message.empty() ||
                        (response.status_code != 200 &&
                         response.status_code != 206)) {
                        failure = response.error.message;
                        if (failure.empty())
                            failure =
                                "http " + std::to_string(response.status_code);
                    } else {
                        finished = true;
                    }
                } else {
                    fclose(file);
                }

                if (state->cancelled.load()) {
                    unlink(dest.c_str());
                    DownloadManager::instance().update(
                        state, [](TaskState& task) {
                            task.state = DownloadState::Cancelled;
                        });
                } else if (!finished) {
                    unlink(dest.c_str());
                    DownloadManager::instance().update(
                        state, [&](TaskState& task) {
                            task.state = DownloadState::Failed;
                            task.error = failure.empty() ? "download failed"
                                                         : failure;
                        });
                } else {
                    DownloadManager::instance().update(
                        state, [&](TaskState& task) {
                            uint64_t size = fileSize(dest);
                            // A transfer that ends early must not pass as done.
                            if (task.total > 0 && size + 1024 < task.total) {
                                task.received = size;
                                task.state = DownloadState::Failed;
                                task.error = "incomplete download";
                            } else {
                                task.received = size;
                                task.total = size;
                                task.state = DownloadState::Done;
                            }
                        });
                }
            });
        });
}

void DownloadManager::retry(uint64_t id) {
    std::shared_ptr<TaskState> target;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& task : tasks) {
            if (task->id != id) continue;
            if (!task->providerRef) return;  // restored history: not retryable
            if (task->state == DownloadState::Queued ||
                task->state == DownloadState::Running)
                return;
            task->cancelled.store(false);
            task->received = 0;
            task->total = 0;
            task->path.clear();
            task->error.clear();
            task->state = DownloadState::Queued;
            target = task;
            break;
        }
    }
    if (target) this->launch(target);
}

void DownloadManager::remove(uint64_t id) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = tasks.begin(); it != tasks.end(); ++it) {
            if ((*it)->id != id) continue;
            if ((*it)->state == DownloadState::Queued ||
                (*it)->state == DownloadState::Running) {
                (*it)->cancelled.store(true);
                if ((*it)->state == DownloadState::Queued)
                    (*it)->state = DownloadState::Cancelled;
            }
            tasks.erase(it);
            break;
        }
    }
    this->saveHistory();
}

void DownloadManager::cancel(uint64_t id) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& task : tasks) {
            if (task->id != id) continue;
            if (task->state == DownloadState::Done ||
                task->state == DownloadState::Failed ||
                task->state == DownloadState::Cancelled)
                return;
            task->cancelled.store(true);
            // Persist right away, otherwise an exit before the worker notices
            // would leave a stale queued record behind.
            task->state = DownloadState::Cancelled;
            changed = true;
            break;
        }
    }
    if (changed) this->saveHistory();
}

void DownloadManager::clearFinished() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        tasks.erase(std::remove_if(tasks.begin(), tasks.end(),
                                   [](const std::shared_ptr<TaskState>& task) {
                                       return task->state == DownloadState::Done ||
                                              task->state == DownloadState::Failed ||
                                              task->state ==
                                                  DownloadState::Cancelled;
                                   }),
                    tasks.end());
    }
    this->saveHistory();
}

std::vector<DownloadTask> DownloadManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<DownloadTask> result;
    result.reserve(tasks.size());
    // Newest first so the task just queued sits at the top of the list.
    for (auto it = tasks.rbegin(); it != tasks.rend(); ++it) {
        const auto& task = *it;
        DownloadTask info;
        info.id = task->id;
        info.name = task->name;
        info.provider = task->provider;
        info.path = task->path;
        info.received = task->received;
        info.total = task->total;
        info.state = task->state;
        info.error = task->error;
        info.retryable = task->providerRef != nullptr;
        result.push_back(std::move(info));
    }
    return result;
}

void DownloadManager::update(const std::shared_ptr<TaskState>& state,
                             std::function<void(TaskState&)> mutate) {
    bool finished = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        mutate(*state);
        finished = state->state == DownloadState::Done ||
                   state->state == DownloadState::Failed ||
                   state->state == DownloadState::Cancelled;
    }
    // Persist history whenever a task reaches a final state.
    if (finished) this->saveHistory();
}

}  // namespace cloud
