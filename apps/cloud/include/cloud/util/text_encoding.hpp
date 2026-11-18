//
// cloud: text encoding helpers for downloaded subtitles.
//

#pragma once

#include <string>

namespace cloud {

/// Converts subtitle bytes to UTF-8. BOMs are honoured, valid UTF-8 is kept
/// as is and anything else is decoded as GBK, which is what most Chinese
/// subtitle releases still ship.
std::string subtitleToUtf8(const std::string& bytes);

}  // namespace cloud
