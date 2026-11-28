//
// cloud: passive "now playing" overlay shown while music plays outside the
// player page.
//

#include "cloud/ui/now_playing_overlay.hpp"

#include <chrono>
#include <cmath>
#include <string>

#include "cloud/drive/music_session.hpp"

using namespace brls::literals;

namespace cloud {

namespace {

/// Cuts `text` back on UTF-8 boundaries until it fits `maxWidth`.
std::string fitText(NVGcontext* vg, const std::string& text, float size,
                    float maxWidth) {
    if (text.empty() || maxWidth <= 0) return "";
    nvgFontSize(vg, size);
    float bounds[4];
    nvgTextBounds(vg, 0, 0, text.c_str(), nullptr, bounds);
    if (bounds[2] - bounds[0] <= maxWidth) return text;

    std::string out = text;
    while (!out.empty()) {
        size_t cut = out.size() - 1;
        while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) cut--;
        out.erase(cut);
        std::string trimmed = out + "…";
        nvgTextBounds(vg, 0, 0, trimmed.c_str(), nullptr, bounds);
        if (bounds[2] - bounds[0] <= maxWidth) return trimmed;
    }
    return "…";
}

}  // namespace

NowPlayingOverlay::NowPlayingOverlay() { this->setFocusable(false); }

void NowPlayingOverlay::draw(NVGcontext* vg, float x, float y, float width,
                             float height, brls::Style style,
                             brls::FrameContext* ctx) {
    MusicSession& session = MusicSession::instance();
    if (!session.active() || width <= 0 || height <= 0) return;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, width, height, 14.0f);
    nvgFillColor(vg, nvgRGBA(12, 14, 22, 212));
    nvgFill(vg);
    nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 24));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    // Three little bars: animated while playing, still when paused.
    const double phase =
        (double)std::chrono::steady_clock::now().time_since_epoch().count() /
        1000000000.0;
    const bool playing = session.playing();
    for (int i = 0; i < 3; i++) {
        float barHeight = 7.0f + (float)i * 4.0f;
        if (playing) barHeight += 3.0f * (float)std::sin(phase * 6.0 + i * 1.7);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x + 18.0f + i * 6.0f,
                       y + height * 0.5f + 8.0f - barHeight, 3.5f, barHeight,
                       1.75f);
        nvgFillColor(vg, nvgRGBA(255, 96, 72, 235));
        nvgFill(vg);
    }

    nvgFontFaceId(vg, brls::Application::getDefaultFont());
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    const float textX = x + 42.0f;
    const float textRight = x + width - 16.0f;
    const float hintWidth = 96.0f;

    std::string title = session.title();
    if (title.empty()) title = brls::getStr("cloud/player/now_playing");
    title = fitText(vg, title, 16.0f, textRight - textX - hintWidth);
    nvgFontSize(vg, 16.0f);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 238));
    nvgText(vg, textX, y + 21.0f, title.c_str(), nullptr);
    // Bundled fonts have no bold face; a hair of offset fakes it.
    nvgText(vg, textX + 0.6f, y + 21.0f, title.c_str(), nullptr);

    const std::string artist =
        fitText(vg, session.artist(), 12.0f, textRight - textX - hintWidth);
    nvgFontSize(vg, 12.0f);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 132));
    nvgText(vg, textX, y + 41.0f, artist.c_str(), nullptr);

    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, nvgRGBA(255, 140, 108, 215));
    nvgText(vg, textRight, y + 41.0f,
            brls::getStr("cloud/player/return_hint").c_str(), nullptr);
}

void attachNowPlayingOverlay(brls::Box* page) {
    if (!page) return;

    NowPlayingOverlay* overlay = new NowPlayingOverlay();
    overlay->setPositionType(brls::PositionType::ABSOLUTE);
    overlay->setPositionRight(40);
    overlay->setPositionBottom(24);
    overlay->setWidth(384);
    overlay->setHeight(60);
    page->addView(overlay);

    // Pages below never use R3, so the action reaches this registration.
    page->registerAction("", brls::BUTTON_RSB, [](brls::View*) -> bool {
        MusicSession::instance().reopen();
        return true;
    });
}

}  // namespace cloud
