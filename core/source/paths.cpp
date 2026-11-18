#include "nxkit/paths.hpp"

#include <sys/stat.h>

#ifndef __SWITCH__
#include <limits.h>
#include <mach-o/dyld.h>

// GLFW chdir()s into the Resources directory on macOS once the app starts,
// so resolve the data directory against the executable instead of the cwd.
static std::string executableDirectory() {
    char raw[PATH_MAX];
    uint32_t size = sizeof(raw);
    if (_NSGetExecutablePath(raw, &size) == 0) {
        std::string path = raw;
        size_t slash = path.find_last_of('/');
        if (slash != std::string::npos) return path.substr(0, slash);
    }
    return ".";
}
#endif

namespace nxkit {

std::string dataDirectory() {
#ifdef __SWITCH__
    return "sdmc:/switch/nxkit/";
#else
    return executableDirectory() + "/nxkit_data/";
#endif
}

std::string dataPath(const std::string& relative) {
    return dataDirectory() + relative;
}

void ensureDirectory(const std::string& path) {
    std::string current;
    for (size_t i = 0; i < path.size(); i++) {
        current += path[i];
        if (path[i] != '/') continue;
        // No trailing slash for mkdir, and skip drive roots like "sdmc:".
        std::string dir = current.substr(0, current.size() - 1);
        if (dir.empty() || dir.back() == ':') continue;
        mkdir(dir.c_str(), 0777);
    }
    // The loop above used to skip the last component, so "a/b/" never
    // created "b". Always create the trimmed path too.
    std::string trimmed = path;
    while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
    if (!trimmed.empty() && trimmed.back() != ':')
        mkdir(trimmed.c_str(), 0777);
}

void ensureDataDirectory() { ensureDirectory(dataDirectory()); }

}  // namespace nxkit
