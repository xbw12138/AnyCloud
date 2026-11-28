//
// cloud: music file naming helpers shared by the player and background session.
//

#pragma once

#include <string>

namespace cloud {

/// A music file name split into its parts, plus the lossless tag.
struct AudioName {
    std::string title;
    std::string artist;
    /// Small label shown in the queue (lossless formats only, "" otherwise).
    std::string tag;
};

/// Splits "Artist - Title.ext" into title / artist and tags lossless formats.
AudioName describeAudio(const std::string& name);

}  // namespace cloud
