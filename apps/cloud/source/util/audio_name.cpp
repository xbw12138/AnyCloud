//
// cloud: music file naming helpers shared by the player and background session.
//

#include "cloud/util/audio_name.hpp"

#include <algorithm>
#include <cctype>

#include <borealis/core/i18n.hpp>

namespace cloud {

AudioName describeAudio(const std::string& name) {
    AudioName info;
    std::string stem = name;
    size_t slash = stem.find_last_of("/\\");
    if (slash != std::string::npos) stem = stem.substr(slash + 1);
    std::string ext;
    size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) {
        ext = stem.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        stem = stem.substr(0, dot);
    }
    info.title = stem;
    size_t dash = stem.find(" - ");
    if (dash != std::string::npos && dash > 0) {
        info.artist = stem.substr(0, dash);
        info.title = stem.substr(dash + 3);
    }
    if (info.title.empty()) info.title = stem;
    if (ext == "flac" || ext == "ape" || ext == "wav" || ext == "alac" ||
        ext == "aiff" || ext == "dsf")
        info.tag = brls::getStr("cloud/player/lossless");
    return info;
}

}  // namespace cloud
