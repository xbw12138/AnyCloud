//
// cloud: provider abstraction for cloud drives.
//

#include "cloud/drive/drive_provider.hpp"

#include <algorithm>

namespace cloud {

namespace {

bool hasExtension(const std::string& name, std::initializer_list<const char*> exts) {
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    for (const char* candidate : exts) {
        if (ext == candidate) return true;
    }
    return false;
}

}  // namespace

MediaKind DriveFile::mediaKind() const {
    if (isDir) return MediaKind::None;
    if (category == 1 ||  // Quark video category
        hasExtension(name, {"mp4", "mkv", "avi", "rmvb", "rm", "flv", "mov",
                            "wmv", "ts", "m2ts", "webm", "mpg", "mpeg", "vob"}))
        return MediaKind::Video;
    if (hasExtension(name, {"mp3", "flac", "aac", "wav", "ogg", "m4a", "ape",
                            "wma", "opus", "dsf"}))
        return MediaKind::Audio;
    if (hasExtension(name, {"jpg", "jpeg", "png", "gif", "bmp", "webp", "heic",
                            "svg", "ico"}))
        return MediaKind::Image;
    return MediaKind::None;
}

}  // namespace cloud
