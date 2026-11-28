//
// cloud: passive "now playing" overlay shown while music plays outside the
// player page.
//

#pragma once

#include <borealis.hpp>

namespace cloud {

/// Small pill in the lower right corner: animated bars, track title, artist and
/// the R3 hint. Draws nothing unless `MusicSession` reports background music.
class NowPlayingOverlay : public brls::View {
public:
    NowPlayingOverlay();

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;
};

/// Adds the overlay to `page` (lower right corner) and binds R3 there to jump
/// back into the audio player. Pages that are full-screen activities call this
/// so the pill is visible everywhere, not only in the tab shell.
void attachNowPlayingOverlay(brls::Box* page);

}  // namespace cloud
