//
// cloud: provider header block helpers (players, viewers, downloads).
//

#pragma once

#include <string>

namespace cloud {

/// Extracts one header from a CRLF separated provider header block.
std::string headerValue(const std::string& headers, const std::string& name);

/// Same block without the given header (mpv gets the user agent through its
/// own property).
std::string headersWithout(const std::string& headers,
                           const std::string& name);

/// mpv wants extra headers as one comma separated list, providers return them
/// CRLF separated (as they would appear in a request).
std::string headersToMpv(const std::string& headers);

/// Starts `url` in mpv with the provider headers applied the way the player
/// does: user agent through its own property, the rest as header fields.
void startMpvStream(const std::string& url, const std::string& headers);

}  // namespace cloud
