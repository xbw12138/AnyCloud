//
// cloud: video / music playback page (libmpv).
//

#include "cloud/pages/player_page.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>

#include <borealis/core/thread.hpp>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "cloud/drive/music_session.hpp"
#include "cloud/drive/playback_store.hpp"
#include "cloud/drive/recent_store.hpp"
#include "cloud/util/audio_name.hpp"
#include "cloud/util/http_headers.hpp"
#include "cloud/util/text_encoding.hpp"
#include "cpr/async.h"
#include "nxkit/http_client.hpp"
#include "nxkit/paths.hpp"
#include "nxkit/utils/number_helper.hpp"

#ifdef NXKIT_WITH_MPV
#include "nxkit/media/mpv_core.hpp"
#endif

using namespace brls::literals;

namespace cloud {

namespace {

/// First UTF-8 character of `text`, used as the disc label glyph.
std::string firstGlyph(const std::string& text) {
    if (text.empty()) return std::string();
    unsigned char lead = (unsigned char)text[0];
    size_t length = lead < 0x80 ? 1 : (lead < 0xE0 ? 2 : (lead < 0xF0 ? 3 : 4));
    if (length > text.size()) length = 1;
    return text.substr(0, length);
}

}  // namespace

/// NanoVG text measurement for the hand-drawn audio layout.
float audioTextWidth(NVGcontext* vg, const std::string& text, float size) {
    if (text.empty()) return 0.0f;
    nvgFontSize(vg, size);
    float bounds[4];
    nvgTextBounds(vg, 0, 0, text.c_str(), nullptr, bounds);
    return bounds[2] - bounds[0];
}

/// Cuts `text` back on UTF-8 boundaries until it fits `maxWidth` and ends it
/// with an ellipsis: long Chinese titles would otherwise run into the tag.
std::string audioFitText(NVGcontext* vg, const std::string& text, float size,
                         float maxWidth) {
    if (maxWidth <= 0) return "";
    if (audioTextWidth(vg, text, size) <= maxWidth) return text;
    std::string out = text;
    while (!out.empty()) {
        size_t cut = out.size() - 1;
        while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) cut--;
        out.erase(cut);
        std::string trimmed = out + "…";
        if (audioTextWidth(vg, trimmed, size) <= maxWidth) return trimmed;
    }
    return "…";
}

/// The bundled fonts only carry a regular face, so the semibold titles of the
/// reference are faked by drawing the same text twice with a hair of offset.
void audioDrawBold(NVGcontext* vg, float x, float y, const std::string& text,
                   float size, NVGcolor color) {
    nvgFontSize(vg, size);
    nvgFillColor(vg, color);
    nvgText(vg, x, y, text.c_str(), nullptr);
    nvgText(vg, x + 0.6f, y, text.c_str(), nullptr);
}

/// Full-screen audio stage, laid out like a widescreen music dashboard: an
/// immersive blue-purple backdrop, a dim spinning record with a thin tonearm on
/// the left and the queue on the right. Everything is painted with NanoVG so
/// the composition never depends on the box layout, and the dark palette stays
/// identical whatever theme the app runs.
class AudioStageView : public brls::View {
public:
    struct Row {
        std::string title;
        std::string artist;
        std::string tag;
    };

    AudioStageView() { this->setFocusable(false); }

    void setQueue(std::vector<Row> rows, size_t index) {
        this->rows = std::move(rows);
        this->index = index;
        this->selected = index;
        this->invalidate();
    }

    void setIndex(size_t value) {
        this->index = value;
        this->selected = value;
        this->invalidate();
    }

    /// Row the D-pad is pointing at; A starts it.
    void setSelection(size_t value) {
        if (this->selected == value) return;
        this->selected = value;
        this->invalidate();
    }

    /// Shuffle on/off and loop mode (0 off, 1 one, 2 all) for the header
    /// badges.
    void setModes(bool shuffleValue, int loopValue) {
        if (this->shuffle == shuffleValue && this->loopMode == loopValue)
            return;
        this->shuffle = shuffleValue;
        this->loopMode = loopValue;
        this->invalidate();
    }

    void setGlyph(std::string value) {
        this->glyph = std::move(value);
        this->invalidate();
    }

    void setPlaying(bool value) {
        if (this->playing == value) return;
        this->playing = value;
        this->invalidate();
    }

    void setProgress(double elapsed, double duration) {
        if (elapsed == this->elapsed && duration == this->duration) return;
        this->elapsed = elapsed;
        this->duration = duration;
        this->invalidate();
    }

    void setStatus(std::string value) {
        if (value == this->status) return;
        this->status = std::move(value);
        this->invalidate();
    }

    void setHint(std::string value) { this->hint = std::move(value); }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (width <= 0 || height <= 0) return;
        nvgFontFaceId(vg, brls::Application::getDefaultFont());
        this->drawBackdrop(vg, x, y, width, height);
        this->drawDisc(vg, x, y, width, height);
        this->drawQueue(vg, x, y, width, height);
        this->drawFooter(vg, x, y, width, height);
    }

private:
    static std::string timeText(double seconds) {
        if (seconds < 0) seconds = 0;
        const int total = (int)seconds;
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d", total / 60, total % 60);
        return buf;
    }

    /// Diagonal blue-grey to deep purple, a cool wash in the upper left, a
    /// purple haze in the lower right and a vignette: at first glance the
    /// layers read like a blurred, frosted cover.
    void drawBackdrop(NVGcontext* vg, float x, float y, float width,
                      float height) {
        NVGpaint base = nvgLinearGradient(vg, x, y, x + width, y + height,
                                          nvgRGB(46, 58, 84),
                                          nvgRGB(36, 27, 50));
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillPaint(vg, base);
        nvgFill(vg);

        NVGpaint cool = nvgRadialGradient(
            vg, x + width * 0.14f, y + height * 0.08f, 0.0f, width * 0.72f,
            nvgRGBA(118, 143, 188, 100), nvgRGBA(118, 143, 188, 0));
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillPaint(vg, cool);
        nvgFill(vg);

        NVGpaint purple = nvgRadialGradient(
            vg, x + width * 0.88f, y + height * 1.04f, 0.0f, width * 0.70f,
            nvgRGBA(98, 61, 134, 115), nvgRGBA(98, 61, 134, 0));
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillPaint(vg, purple);
        nvgFill(vg);

        NVGpaint wash = nvgRadialGradient(
            vg, x + width * 0.24f, y + height * 0.52f, 0.0f, width * 0.30f,
            nvgRGBA(255, 255, 255, 16), nvgRGBA(255, 255, 255, 0));
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillPaint(vg, wash);
        nvgFill(vg);

        NVGpaint vignette = nvgRadialGradient(
            vg, x + width * 0.5f, y + height * 0.5f, height * 0.34f,
            height * 0.98f, nvgRGBA(6, 8, 16, 0), nvgRGBA(6, 8, 16, 118));
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillPaint(vg, vignette);
        nvgFill(vg);
    }

    void drawDisc(NVGcontext* vg, float x, float y, float width,
                  float height) {
        const auto now = std::chrono::steady_clock::now();
        if (this->last.time_since_epoch().count() != 0) {
            double dt =
                std::chrono::duration<double>(now - this->last).count();
            if (dt > 0.25) dt = 0.25;
            if (this->playing) this->angle += (float)(dt * 8.0);
            if (this->angle >= 360.0f) this->angle -= 360.0f;
        }
        this->last = now;

        const float r = height * 0.215f;
        const float cx = x + width * 0.255f;
        const float cy = y + height * 0.545f;

        NVGpaint bloom = nvgRadialGradient(
            vg, cx, cy, r * 0.20f, r * 1.75f, nvgRGBA(152, 174, 216, 42),
            nvgRGBA(152, 174, 216, 0));
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, r * 1.75f);
        nvgFillPaint(vg, bloom);
        nvgFill(vg);

        // Translucent vinyl: the backdrop keeps glowing through the record.
        NVGpaint body = nvgRadialGradient(
            vg, cx - r * 0.25f, cy - r * 0.30f, r * 0.05f, r * 1.05f,
            nvgRGBA(33, 42, 64, 120), nvgRGBA(10, 13, 24, 178));
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, r);
        nvgFillPaint(vg, body);
        nvgFill(vg);

        // Grooves and label rotate together.
        nvgSave(vg);
        nvgTranslate(vg, cx, cy);
        nvgRotate(vg, NVG_PI / 180.0f * this->angle);
        for (int i = 0; i < 18; i++) {
            nvgBeginPath(vg);
            nvgCircle(vg, 0, 0, r * (0.36f + 0.034f * (float)i));
            nvgStrokeColor(vg, nvgRGBA(255, 255, 255, i % 3 == 0 ? 15 : 8));
            nvgStrokeWidth(vg, 1.0f);
            nvgStroke(vg);
        }
        nvgBeginPath(vg);
        nvgCircle(vg, 0, 0, r * 0.34f);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 12));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, 0, 0, r * 0.34f);
        nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 20));
        nvgStrokeWidth(vg, 1.0f);
        nvgStroke(vg);
        if (!this->glyph.empty()) {
            nvgFontSize(vg, r * 0.24f);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg, nvgRGBA(255, 255, 255, 34));
            nvgText(vg, 0, 2.0f, this->glyph.c_str(), nullptr);
        }
        nvgRestore(vg);

        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, r);
        nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 24));
        nvgStrokeWidth(vg, 1.2f);
        nvgStroke(vg);

        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, r * 0.018f);
        nvgFillColor(vg, nvgRGBA(8, 10, 18, 200));
        nvgFill(vg);

        // Glossy sweep over the upper left of the vinyl.
        NVGpaint sheen = nvgRadialGradient(
            vg, cx - r * 0.35f, cy - r * 0.40f, 1.0f, r * 0.92f,
            nvgRGBA(255, 255, 255, 32), nvgRGBA(255, 255, 255, 0));
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, r);
        nvgFillPaint(vg, sheen);
        nvgFill(vg);

        // Tonearm: a slim arm from above the record, dot stylus on the groove.
        const float pivotX = cx + r * 0.55f;
        const float pivotY = cy - r * 2.05f;
        const float tipX = cx + r * 0.34f;
        const float tipY = cy - r * 0.78f;
        nvgBeginPath(vg);
        nvgMoveTo(vg, pivotX, pivotY);
        nvgLineTo(vg, tipX, tipY);
        nvgStrokeColor(vg, nvgRGBA(226, 232, 244, 150));
        nvgStrokeWidth(vg, 1.8f);
        nvgLineCap(vg, NVG_ROUND);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, pivotX, pivotY, 4.5f);
        nvgStrokeColor(vg, nvgRGBA(226, 232, 244, 165));
        nvgStrokeWidth(vg, 1.6f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, tipX, tipY, 3.2f);
        nvgFillColor(vg, nvgRGBA(236, 240, 248, 220));
        nvgFill(vg);
        nvgLineCap(vg, NVG_BUTT);

        if (this->playing) return;

        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, r);
        nvgFillColor(vg, nvgRGBA(8, 10, 18, 90));
        nvgFill(vg);
        const float s = r * 0.13f;
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx - s * 0.5f, cy - s);
        nvgLineTo(vg, cx + s * 1.0f, cy);
        nvgLineTo(vg, cx - s * 0.5f, cy + s);
        nvgClosePath(vg);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 200));
        nvgFill(vg);
    }

    /// "当前播放 (147)" plus the rows around the selected entry: number, white
    /// title, orange tag, dim artist. The row under the D-pad is highlighted,
    /// the playing row gets a white outline and an animated equalizer.
    void drawQueue(NVGcontext* vg, float x, float y, float width,
                   float height) {
        const float listX = x + width * 0.515f;
        const float listW = width * 0.425f;
        const float headerY = y + height * 0.100f;
        const float rowH = height * 0.0736f;
        const float firstY = y + height * 0.165f;
        const int visible = 10;

        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        const std::string heading = brls::getStr("cloud/player/now_playing");
        audioDrawBold(vg, listX, headerY, heading, 26.0f,
                      nvgRGBA(255, 255, 255, 238));
        const float headingWidth = audioTextWidth(vg, heading, 26.0f) + 0.6f;

        nvgFontSize(vg, 16.0f);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 96));
        const std::string count = "(" + std::to_string(this->rows.size()) + ")";
        nvgText(vg, listX + headingWidth + 14.0f, headerY + 1.5f,
                count.c_str(), nullptr);

        // Mode badges on the right of the header: shuffle and loop state.
        float badgeRight = listX + listW;
        auto drawBadge = [&](const std::string& text, NVGcolor fill,
                             NVGcolor color) {
            const float fontSize = 12.0f;
            const float widthOf = audioTextWidth(vg, text, fontSize) + 18.0f;
            badgeRight -= widthOf;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, badgeRight, headerY - 10.5f, widthOf, 21.0f,
                           10.5f);
            nvgFillColor(vg, fill);
            nvgFill(vg);
            nvgFontSize(vg, fontSize);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg, color);
            nvgText(vg, badgeRight + widthOf * 0.5f, headerY + 0.5f,
                    text.c_str(), nullptr);
            badgeRight -= 8.0f;
        };
        if (this->loopMode != 0) {
            drawBadge(brls::getStr(this->loopMode == 1
                                       ? "cloud/player/badge_loop_one"
                                       : "cloud/player/badge_loop_all"),
                      nvgRGBA(255, 255, 255, 22),
                      nvgRGBA(255, 255, 255, 190));
        }
        if (this->shuffle) {
            drawBadge("cloud/player/badge_shuffle"_i18n,
                      nvgRGBA(255, 92, 60, 40), nvgRGBA(255, 140, 108, 235));
        }

        if (this->rows.empty()) return;

        // Keep the D-pad selection on the third visible row while scrolling.
        int first = 0;
        if (this->selected > 2) first = (int)this->selected - 2;
        if (first + visible > (int)this->rows.size())
            first = (int)this->rows.size() > visible
                        ? (int)this->rows.size() - visible
                        : 0;

        for (int i = 0; i < visible; i++) {
            const int rowIndex = first + i;
            if (rowIndex >= (int)this->rows.size()) break;
            const Row& row = this->rows[rowIndex];
            const bool current = (size_t)rowIndex == this->index;
            const bool selectedRow = (size_t)rowIndex == this->selected;
            const float yc = firstY + rowH * (float)i;

            if (current) {
                nvgBeginPath(vg);
                nvgRoundedRect(vg, listX - 16.0f, yc - rowH * 0.42f,
                               listW + 16.0f, rowH * 0.84f, 9.0f);
                nvgFillColor(vg, nvgRGBA(255, 255, 255, 14));
                nvgFill(vg);
                nvgStrokeColor(vg, nvgRGBA(255, 255, 255, selectedRow ? 235 : 200));
                nvgStrokeWidth(vg, 1.6f);
                nvgStroke(vg);

                // Three little bars where the row number sits: they dance
                // while the track plays and freeze when paused.
                const double phase =
                    (double)std::chrono::steady_clock::now()
                        .time_since_epoch()
                        .count() /
                    1000000000.0;
                const float baseX = listX + 2.0f;
                const float heights[3] = {7.0f, 11.0f, 8.5f};
                for (int b = 0; b < 3; b++) {
                    float h = heights[b];
                    if (this->playing)
                        h += 3.5f * (float)std::sin(phase * 6.0 + b * 1.9);
                    nvgBeginPath(vg);
                    nvgRoundedRect(vg, baseX + b * 6.0f, yc + 7.0f - h, 4.0f,
                                   h, 2.0f);
                    nvgFillColor(vg, nvgRGBA(255, 84, 112, 255));
                    nvgFill(vg);
                }
            } else {
                if (selectedRow) {
                    nvgBeginPath(vg);
                    nvgRoundedRect(vg, listX - 16.0f, yc - rowH * 0.40f,
                                   listW + 16.0f, rowH * 0.80f, 9.0f);
                    nvgFillColor(vg, nvgRGBA(255, 255, 255, 20));
                    nvgFill(vg);
                }
                nvgFontSize(vg, 15.0f);
                nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
                nvgFillColor(vg, nvgRGBA(255, 255, 255,
                                         selectedRow ? 190 : 105));
                const std::string number = std::to_string(rowIndex + 1);
                nvgText(vg, listX + 24.0f, yc, number.c_str(), nullptr);
            }

            const float titleX = listX + 44.0f;
            const float titleSize = 21.0f;
            const float tagSize = 11.5f;
            const float artistSize = 14.5f;

            const float tagW =
                row.tag.empty() ? 0.0f : audioTextWidth(vg, row.tag, tagSize) + 16.0f;
            const std::string artistText =
                row.artist.empty() ? std::string() : "- " + row.artist;
            const float artistW =
                row.artist.empty()
                    ? 0.0f
                    : audioTextWidth(vg, artistText, artistSize) + 12.0f;
            const float titleMax = listX + listW - titleX - tagW - artistW -
                                   (row.tag.empty() ? 0.0f : 12.0f);
            const std::string title =
                audioFitText(vg, row.title, titleSize, titleMax);

            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            audioDrawBold(vg, titleX, yc, title, titleSize,
                          nvgRGBA(255, 255, 255,
                                  (current || selectedRow) ? 255 : 222));
            float cursor = titleX + audioTextWidth(vg, title, titleSize) + 0.6f;

            if (!row.tag.empty()) {
                cursor += 12.0f;
                nvgBeginPath(vg);
                nvgRoundedRect(vg, cursor, yc - 9.5f, tagW, 19.0f, 9.5f);
                nvgFillColor(vg, nvgRGBA(255, 92, 60, 34));
                nvgFill(vg);
                nvgFontSize(vg, tagSize);
                nvgFillColor(vg, nvgRGBA(255, 118, 84, 235));
                nvgText(vg, cursor + 8.0f, yc + 0.5f, row.tag.c_str(), nullptr);
                cursor += tagW;
            }

            if (!row.artist.empty()) {
                nvgFontSize(vg, artistSize);
                nvgFillColor(vg, nvgRGBA(255, 255, 255, 110));
                nvgText(vg, cursor + 12.0f, yc + 1.0f, artistText.c_str(),
                        nullptr);
            }
        }
    }

    /// Under the record: transient status, a hairline progress bar with the
    /// elapsed / total times, and the key hints across the bottom.
    void drawFooter(NVGcontext* vg, float x, float y, float width,
                    float height) {
        const float r = height * 0.215f;
        const float cx = x + width * 0.255f;

        if (!this->status.empty()) {
            nvgFontSize(vg, 13.5f);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg, nvgRGBA(255, 255, 255, 150));
            nvgText(vg, cx, y + height * 0.795f, this->status.c_str(), nullptr);
        }

        const float barY = y + height * 0.848f;
        const float half = r * 0.92f;
        nvgFontSize(vg, 12.5f);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 120));
        nvgText(vg, cx - half - 12.0f, barY, timeText(this->elapsed).c_str(),
                nullptr);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(vg, cx + half + 12.0f, barY,
                (this->duration > 0 ? timeText(this->duration) : "--:--").c_str(),
                nullptr);

        nvgBeginPath(vg);
        nvgRoundedRect(vg, cx - half, barY - 1.5f, half * 2.0f, 3.0f, 1.5f);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 32));
        nvgFill(vg);

        if (this->duration > 0) {
            float progress = (float)(this->elapsed / this->duration);
            if (progress < 0) progress = 0;
            if (progress > 1) progress = 1;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, cx - half, barY - 1.5f,
                           std::max(half * 2.0f * progress, 3.0f), 3.0f, 1.5f);
            nvgFillColor(vg, nvgRGBA(255, 120, 88, 220));
            nvgFill(vg);
        }

        if (!this->hint.empty()) {
            nvgFontSize(vg, 11.5f);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg, nvgRGBA(255, 255, 255, 74));
            nvgText(vg, x + width * 0.5f, y + height * 0.955f,
                    this->hint.c_str(), nullptr);
        }
    }

    std::vector<Row> rows;
    size_t index = 0;
    size_t selected = 0;
    bool shuffle = false;
    int loopMode = 0;
    std::string glyph;
    std::string status;
    std::string hint;
    bool playing = true;
    double elapsed = 0;
    double duration = 0;
    float angle = 0.0f;
    std::chrono::steady_clock::time_point last{};
};

namespace {

// Temporary on-device diagnostics, same file the QR view logs to.
void logDebug(const std::string& line) {
#ifdef __SWITCH__
    FILE* file = fopen("sdmc:/switch/nxkit/qr_debug.log", "a");
#else
    FILE* file = fopen("/tmp/qr_debug.log", "a");
#endif
    if (!file) return;
    fprintf(file, "%s\n", line.c_str());
    fclose(file);
}

#ifdef __SWITCH__
/// Console volume, through the same service the quick settings slider uses,
/// so ↑/↓ behave like the system volume keys instead of only scaling mpv.
/// Falls back to silence on failure: the caller then adjusts mpv instead.
class SystemVolumeControl {
public:
    static SystemVolumeControl& instance() {
        static SystemVolumeControl control;
        return control;
    }

    /// Returns the new volume as a percentage, or -1 when the system refused.
    int step(int delta) {
        if (!this->ready) return -1;
        AudioTarget target = AudioTarget_Invalid;
        if (R_FAILED(audctlGetActiveOutputTarget(&target)) ||
            target == AudioTarget_Invalid)
            target = AudioTarget_Speaker;

        s32 current = 0;
        s32 low = 0;
        s32 high = 100;
        if (R_FAILED(audctlGetTargetVolume(&current, target)) ||
            R_FAILED(audctlGetTargetVolumeMin(&low)) ||
            R_FAILED(audctlGetTargetVolumeMax(&high)) ||
            high <= low)
            return -1;

        // Step by whole percent of the range so the OSD number matches the
        // slider the user sees in the system menu.
        const int span = (int)(high - low);
        const int stepSize = span >= 20 ? span / 20 : 1;
        int value = (int)current + (delta > 0 ? stepSize : -stepSize);
        if (value < (int)low) value = (int)low;
        if (value > (int)high) value = (int)high;
        if (R_FAILED(audctlSetTargetVolume(target, (s32)value))) return -1;
        return (value - (int)low) * 100 / span;
    }

private:
    SystemVolumeControl() {
        this->ready = R_SUCCEEDED(audctlInitialize());
        logDebug(std::string("player: audctl ") +
                 (this->ready ? "ready" : "unavailable"));
    }

    bool ready = false;
};
#endif

std::string speedText(double speed) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%gx", speed);
    return buf;
}

std::string extensionOf(const std::string& name) {
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

bool isSubtitleName(const std::string& name) {
    std::string ext = extensionOf(name);
    return ext == "srt" || ext == "ass" || ext == "ssa" || ext == "vtt";
}

/// Name without the extension, lowercased: "Movie.CHS.srt" -> "movie.chs".
std::string stemOf(const std::string& name) {
    size_t slash = name.find_last_of("/\\");
    std::string base =
        slash == std::string::npos ? name : name.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    std::transform(base.begin(), base.end(), base.begin(), ::tolower);
    return base;
}

/// "Movie" matches "Movie.srt", "Movie.chs.srt" and "Movie-简中.ass", but not
/// "Movie2.srt".
bool subtitleMatchesVideo(const std::string& videoStem,
                          const std::string& subName) {
    std::string stem = stemOf(subName);
    if (stem == videoStem) return true;
    if (stem.size() <= videoStem.size()) return false;
    if (stem.compare(0, videoStem.size(), videoStem) != 0) return false;
    char next = stem[videoStem.size()];
    return next == '.' || next == ' ' || next == '-' || next == '_';
}

struct AspectOption {
    /// i18n key, or a literal ratio for the fixed entries.
    const char* label;
    /// mpv `video-aspect-override` value; nullptr means "stretch to fill".
    const char* value;
};

const AspectOption kAspects[] = {
    {"cloud/player/aspect_default", "no"},
    {"16:9", "16:9"},
    {"4:3", "4:3"},
    {"1:1", "1:1"},
    {"21:9", "21:9"},
    {"cloud/player/aspect_stretch", nullptr},
};
const int kAspectCount = (int)(sizeof(kAspects) / sizeof(kAspects[0]));

std::string aspectLabel(const AspectOption& option) {
    if (strncmp(option.label, "cloud/", 6) == 0)
        return brls::getStr(option.label);
    return option.label;
}

/// Random generator shared by every shuffle round of the session.
std::mt19937& audioRng() {
    static std::mt19937 rng(
        (unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
    return rng;
}

}  // namespace

void SeekBarView::draw(NVGcontext* vg, float x, float y, float width,
                       float height, brls::Style style,
                       brls::FrameContext* ctx) {
    if (width <= 0 || height <= 0) return;

    const float centerY = y + height * 0.5f;
    const float trackHeight = 5.0f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, centerY - trackHeight * 0.5f, width, trackHeight,
                   trackHeight * 0.5f);
    nvgFillColor(vg, this->trackColor);
    nvgFill(vg);

    const float filled = width * this->progress;
    if (filled <= 0) return;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, centerY - trackHeight * 0.5f,
                   std::max(filled, trackHeight), trackHeight,
                   trackHeight * 0.5f);
    nvgFillColor(vg, brls::Application::getTheme()["brls/accent"]);
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgCircle(vg, x + filled, centerY, 7.0f);
    nvgFillColor(vg, this->thumbColor);
    nvgFill(vg);
}

PlayerPage::PlayerPage(std::shared_ptr<DriveProvider> provider,
                       std::vector<DriveFile> playlist, size_t index,
                       bool isAudio, bool resumeInPlace)
    : provider(std::move(provider)),
      isAudio(isAudio),
      playlist(std::move(playlist)),
      playlistIndex(index),
      resumeInPlace(resumeInPlace) {
    if (this->playlist.empty()) this->playlist.push_back(DriveFile{});
    if (this->playlistIndex >= this->playlist.size())
        this->playlistIndex = this->playlist.size() - 1;
    this->file = this->playlist[this->playlistIndex];
    logDebug("player: open '" + this->file.name + "' idx=" +
             std::to_string(this->playlistIndex) + "/" +
             std::to_string(this->playlist.size()) +
             (this->isAudio ? " audio" : " video"));

    this->setAxis(brls::Axis::COLUMN);
    this->setGrow(1);
    // Take focus (and hide the highlight box) so the file list cell that
    // opened us does not keep drawing its focus ring over the video.
    this->setFocusable(true);
    this->setHideHighlight(true);

#ifdef __SWITCH__
    // Report the console as playing media, otherwise the screen dims and the
    // system sleep timer eventually kicks in while the video keeps playing.
    brls::Application::getPlatform()->disableScreenDimming(
        true, "media playback", "cloud");
#endif

#ifdef NXKIT_WITH_MPV
    this->mpvAvailable = true;
#endif
    // Continue where the previous session stopped.
    this->resumePosition = PlaybackStore::instance().position(this->file.id);

    if (this->isAudio) {
        // The stage covers the whole page and paints its own dark blue-purple
        // backdrop, the record and the queue. Absolute NanoVG painting keeps
        // the composition independent of the box layout and of the app theme.
        this->setAxis(brls::Axis::COLUMN);
        this->setAlignItems(brls::AlignItems::STRETCH);
        this->setBackgroundColor(nvgRGBA(0, 0, 0, 0));

        std::vector<AudioStageView::Row> rows;
        rows.reserve(this->playlist.size());
        for (const auto& entry : this->playlist) {
            AudioName name = describeAudio(entry.name);
            rows.push_back({name.title, name.artist, name.tag});
        }
        AudioName current = describeAudio(this->file.name);

        this->stage = new AudioStageView();
        this->stage->setGrow(1);
        this->stage->setQueue(std::move(rows), this->playlistIndex);
        this->stage->setGlyph(firstGlyph(current.title));
        this->stage->setHint("cloud/player/hint_audio"_i18n);
        this->stage->setModes(this->shuffle, (int)this->loopMode);
        this->stage->setStatus("cloud/player/loading"_i18n);
        this->addView(this->stage);

        // Publish the track so the shell can show its "now playing" overlay
        // once this page is closed.
        MusicSession::instance().attach();
        MusicSession::instance().setTrack(current.title, current.artist,
                                          this->provider, this->playlist,
                                          this->playlistIndex);
        if (this->resumeInPlace) {
            // mpv keeps playing this track: no seek, the queue cursor starts
            // on the row that is actually running.
            this->resumePosition = 0;
            this->selectedIndex = this->playlistIndex;
        }
    } else {
        // A video replaces whatever music was playing in the background.
        MusicSession::instance().clear();
        // Clean overlay without any dark gradient: only the title at the top,
        // the progress bar pinned to the bottom edge and a small status / key
        // line above it, so the picture stays untouched.
        this->osdPanel = new brls::Box(brls::Axis::COLUMN);
        this->osdPanel->setGrow(1);
        this->osdPanel->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        this->osdPanel->setAlignItems(brls::AlignItems::STRETCH);
        this->osdPanel->setPadding(26, 44, 22, 44);

        this->osdName = new brls::Label();
        this->osdName->setText(this->file.name);
        this->osdName->setFontSize(26);
        this->osdName->setSingleLine(true);
        this->osdName->setTextColor(nvgRGBA(255, 255, 255, 255));
        this->osdPanel->addView(this->osdName);

        brls::Box* bottom = new brls::Box(brls::Axis::COLUMN);
        bottom->setAlignItems(brls::AlignItems::STRETCH);

        this->metaLabel = new brls::Label();
        this->metaLabel->setFontSize(15);
        this->metaLabel->setSingleLine(true);
        this->metaLabel->setTextColor(nvgRGBA(255, 255, 255, 185));
        bottom->addView(this->metaLabel);

        this->hintLabel = new brls::Label();
        this->hintLabel->setFontSize(13);
        this->hintLabel->setMarginTop(4);
        this->hintLabel->setSingleLine(true);
        this->hintLabel->setTextColor(nvgRGBA(255, 255, 255, 145));
        this->hintLabel->setText("cloud/player/hint_video"_i18n);
        bottom->addView(this->hintLabel);

        brls::Box* progressRow = new brls::Box(brls::Axis::ROW);
        progressRow->setAlignItems(brls::AlignItems::CENTER);
        progressRow->setMarginTop(10);

        this->elapsedLabel = new brls::Label();
        this->elapsedLabel->setFontSize(16);
        this->elapsedLabel->setTextColor(nvgRGBA(255, 255, 255, 225));
        this->elapsedLabel->setText("00:00");
        progressRow->addView(this->elapsedLabel);

        this->seekBar = new SeekBarView();
        this->seekBar->setGrow(1);
        this->seekBar->setHeight(18);
        this->seekBar->setMargins(0, 14, 0, 14);
        progressRow->addView(this->seekBar);

        this->durationLabel = new brls::Label();
        this->durationLabel->setFontSize(16);
        this->durationLabel->setTextColor(nvgRGBA(255, 255, 255, 225));
        this->durationLabel->setText("--:--");
        progressRow->addView(this->durationLabel);

        bottom->addView(progressRow);
        this->osdPanel->addView(bottom);

        this->osdPanel->setVisibility(brls::Visibility::GONE);
        this->addView(this->osdPanel);
    }

#ifdef NXKIT_WITH_MPV
    // A: pause / resume. Left/Right: seek. B: exit.
    this->registerAction(
        "", brls::BUTTON_A,
        [this](brls::View*) -> bool {
            // Audio: A starts the row the D-pad points at; on the playing row
            // it toggles pause like everywhere else.
            if (this->isAudio && this->selectedIndex != this->playlistIndex) {
                this->playAt(this->selectedIndex);
                return true;
            }
            auto& mpv = nxkit::MPVCore::instance();
            if (mpv.isPaused())
                mpv.resume();
            else
                mpv.pause();
            this->updateOsd();
            // Keep the OSD on screen while paused, fade it once playing.
            this->showOsd(!mpv.isPaused());
            return true;
        },
        true);
    this->registerAction(
        "", brls::BUTTON_LEFT,
        [this](brls::View*) -> bool {
            this->seekBy(-10);
            return true;
        },
        true, true);
    this->registerAction(
        "", brls::BUTTON_RIGHT,
        [this](brls::View*) -> bool {
            this->seekBy(10);
            return true;
        },
        true, true);
    this->registerAction(
        "", brls::BUTTON_DOWN,
        [this](brls::View*) -> bool {
            if (this->isAudio)
                this->moveSelection(1);
            else
                this->adjustVolume(-5);
            return true;
        },
        true, true);
    this->registerAction(
        "", brls::BUTTON_UP,
        [this](brls::View*) -> bool {
            if (this->isAudio)
                this->moveSelection(-1);
            else
                this->adjustVolume(5);
            return true;
        },
        true, true);
    this->registerAction(
        "", brls::BUTTON_B,
        [this](brls::View*) -> bool {
            logDebug("player: exit key, back to list");
            // Remember the position before tearing the player down.
            this->savePosition();
            // Audio is handed over to the background: only the page closes,
            // mpv keeps decoding. Videos always stop on exit.
            if (!this->isAudio) {
                auto& mpv = nxkit::MPVCore::instance();
                mpv.stop();
                mpv.reset();
            }
            brls::Application::popActivity();
            return true;
        },
        true);

    this->registerAction(
        "cloud/player/prev"_i18n, brls::BUTTON_LB,
        [this](brls::View*) -> bool {
            this->playPreviousManual();
            return true;
        });
    this->registerAction(
        "cloud/player/next"_i18n, brls::BUTTON_RB,
        [this](brls::View*) -> bool {
            this->playNextManual();
            return true;
        });
    this->registerAction(
        "cloud/player/loop"_i18n, brls::BUTTON_X,
        [this](brls::View*) -> bool {
            this->toggleLoopMode();
            return true;
        });
    // Plus opens the directory of the current video.
    this->registerAction(
        "cloud/player/playlist"_i18n, brls::BUTTON_START,
        [this](brls::View*) -> bool {
            this->showPlaylistMenu();
            return true;
        });
    // ZL / ZR jump half a minute on video; on the audio page they adjust the
    // volume, since ↑/↓ move the queue there.
    this->registerAction(
        "", brls::BUTTON_LT,
        [this](brls::View*) -> bool {
            if (this->isAudio)
                this->adjustVolume(-5);
            else
                this->seekBy(-30);
            return true;
        },
        true, true);
    this->registerAction(
        "", brls::BUTTON_RT,
        [this](brls::View*) -> bool {
            if (this->isAudio)
                this->adjustVolume(5);
            else
                this->seekBy(30);
            return true;
        },
        true, true);

    if (!this->isAudio) {
        this->registerAction(
            "cloud/player/quality"_i18n, brls::BUTTON_Y,
            [this](brls::View*) -> bool {
                this->showOsd();
                this->showQualityMenu();
                return true;
            });
        // "-" (BUTTON_BACK) opens the track list: embedded subtitles, the
        // subtitle files next to the video and the embedded audio tracks. It is
        // the only free button next to the ones already used by the player.
        this->registerAction(
            "cloud/player/track"_i18n, brls::BUTTON_BACK,
            [this](brls::View*) -> bool {
                this->showOsd();
                this->showTrackMenu();
                return true;
            });
        // R3 switches the displayed aspect ratio (4:3 material on a 16:9
        // screen, anamorphic sources, stretched full-screen fills, ...).
        this->registerAction(
            "cloud/player/aspect"_i18n, brls::BUTTON_RSB,
            [this](brls::View*) -> bool {
                this->showOsd();
                this->showAspectMenu();
                return true;
            });
    } else {
        // Audio: Y toggles shuffle.
        this->registerAction(
            "cloud/player/shuffle"_i18n, brls::BUTTON_Y,
            [this](brls::View*) -> bool {
                this->toggleShuffle();
                return true;
            });
    }

    // L3 (left stick click) opens the playback speed menu; works for audio too.
    this->registerAction(
        "cloud/player/speed"_i18n, brls::BUTTON_LSB,
        [this](brls::View*) -> bool {
            this->showOsd();
            this->showSpeedMenu();
            return true;
        });

    this->eventSub = nxkit::MPVCore::instance().getEvent()->subscribe(
        [this, alive = this->alive](nxkit::MpvEventEnum event) {
            brls::sync([this, alive, event] {
                if (!*alive) return;
                this->onMpvEvent((int)event);
            });
        });
#endif

    this->play();
}

PlayerPage::~PlayerPage() {
    logDebug("player: destroy '" + this->file.name + "'");
    *this->alive = false;
    if (this->osdTimer) {
        brls::cancelDelay(this->osdTimer);
        this->osdTimer = 0;
    }
    if (this->volumeTimer) {
        brls::cancelDelay(this->volumeTimer);
        this->volumeTimer = 0;
    }
    this->savePosition();
#ifdef __SWITCH__
    brls::Application::getPlatform()->disableScreenDimming(
        false, "media playback", "cloud");
#endif
    if (this->isAudio) {
#ifdef NXKIT_WITH_MPV
        // Ending on the last track closes the page with mpv stopped: nothing
        // plays in the background then. Otherwise the session takes over and
        // advances to the next track when this one ends.
        if (nxkit::MPVCore::instance().isStopped())
            MusicSession::instance().clear();
        else
            MusicSession::instance().background(this->shuffle,
                                                (int)this->loopMode);
#else
        MusicSession::instance().clear();
#endif
    }
#ifdef NXKIT_WITH_MPV
    auto& mpv = nxkit::MPVCore::instance();
    mpv.getEvent()->unsubscribe(this->eventSub);
    // Music keeps playing after the page is closed, so browsing the library
    // does not cut the track. Anything that starts a new stream replaces it.
    if (!this->isAudio) {
        mpv.stop();
        mpv.reset();
    }
#endif
}

void PlayerPage::play() {
    if (!this->mpvAvailable) {
        this->setStatus("cloud/player/unavailable"_i18n);
        return;
    }
#ifdef NXKIT_WITH_MPV
    if (this->isAudio) {
        // Reopened from the background overlay: mpv is still decoding this
        // track, so taking the stream over avoids the silence of resolving the
        // url and buffering it again. One-shot: switching tracks afterwards
        // has to load the new stream normally.
        if (this->resumeInPlace) {
            this->resumeInPlace = false;
            auto& mpv = nxkit::MPVCore::instance();
            if (!mpv.isStopped()) {
                this->updateOsd();
                return;
            }
        }
        this->playOriginal();
        return;
    }
    // Subtitles live next to the video, fetch the list while the stream is
    // negotiated so both requests run in parallel.
    this->requestSubtitles();
    // Prefer transcoded streams (no special headers required).
    logDebug("player: requesting streams");
    // Switching entries clears the stream list while the previous request may
    // still be in flight. Remember which file this answer belongs to so a late
    // reply can never start the old video again.
    const std::string fileId = this->file.id;
    this->provider->videoStreams(
        this->file.id,
        [this, alive = this->alive, fileId](
            const std::vector<VideoStream>& streams, const std::string& def,
            const std::string& error) {
            brls::sync([this, alive, fileId, streams, def, error] {
                if (!*alive) return;
                if (this->file.id != fileId) return;
                logDebug("player: streams n=" + std::to_string(streams.size()) +
                         " err=" + error);
                if (!error.empty() || streams.empty()) {
                    this->playOriginal();
                    return;
                }
                this->streams = streams;
                this->streamIndex = 0;
                for (size_t i = 0; i < streams.size(); i++) {
                    if (streams[i].resolution == def) {
                        this->streamIndex = (int)i;
                        break;
                    }
                }
                this->startUrl(streams[this->streamIndex].url,
                               this->provider->playbackHeaders());
            });
        });
#endif
}

void PlayerPage::playOriginal() {
    logDebug("player: requesting download info");
    const std::string fileId = this->file.id;
    this->provider->downloadInfo(
        this->file.id,
        [this, alive = this->alive, fileId](
            const std::string& url, const std::string& headers,
            const std::string& error) {
            brls::sync([this, alive, fileId, url, headers, error] {
                if (!*alive) return;
                if (this->file.id != fileId) return;
                logDebug("player: download info err=" + error +
                         " urlLen=" + std::to_string(url.size()));
                if (!error.empty() || url.empty()) {
                    this->setStatus(
                        error.empty() ? "cloud/player/no_stream"_i18n : error);
                    return;
                }
                // Streaming headers differ from download headers for some
                // drives (Baidu's CDN rejects the netdisk UA with 403 but the
                // same link works with a browser identity).
                std::string playback = this->provider->playbackHeaders();
                // Some Baidu CDN nodes still require the account cookie and
                // netdisk UA; keep the download headers as a one-shot retry.
                this->fallbackHeaders =
                    (this->provider->id() == "baidu" && !headers.empty())
                        ? headers
                        : std::string();
                this->triedFallbackHeaders = false;
                this->startUrl(url, playback.empty() ? headers : playback);
            });
        });
}

bool PlayerPage::playAt(size_t index) {
    if (index >= this->playlist.size()) return false;

    this->playlistIndex = index;
    this->selectedIndex = index;
    // The started track leaves the current shuffle round.
    for (auto it = this->shuffleBag.begin(); it != this->shuffleBag.end();
         ++it) {
        if (*it == index) {
            this->shuffleBag.erase(it);
            break;
        }
    }
    this->file = this->playlist[index];
    // Stamp the drive the file is played from; the player may have been started
    // for a drive that is not the currently active one (recent views).
    if (this->file.providerId.empty() && this->provider) {
        this->file.providerId = this->provider->id();
        this->file.provider = this->provider->name();
    }
    // Local files are opened by path, they cannot be resolved back through a
    // drive later, so they never enter the recent list.
    if (this->provider && this->provider->id() != "local")
        RecentStore::instance().add(this->file);
    logDebug("player: play index " + std::to_string(index) + " '" +
             this->file.name + "'");

    // Start the entry from scratch: drop everything tied to the previous one
    // so its quality list or resume position cannot leak into it.
    this->resumePosition = PlaybackStore::instance().position(this->file.id);
    this->streams.clear();
    this->streamIndex = -1;
    this->lastUrl.clear();
    this->lastHeaders.clear();
    this->fallbackHeaders.clear();
    this->triedFallbackHeaders = false;
    this->triedSoftwareDec = false;
    this->triedLocalCache = false;
    this->statusOverride = false;
    // Subtitles belong to the video that was playing, not to the new one.
    this->subtitles.clear();
    this->subtitleIndex = -1;
    this->audioTracks.clear();
    this->embeddedSubtitles.clear();
    this->activeSubtitlePath.clear();
    this->pendingSubtitlePath.clear();

    if (this->titleLabel) this->titleLabel->setText(this->file.name);
    if (this->osdName) this->osdName->setText(this->file.name);
    if (this->seekBar) this->seekBar->setProgress(0);
    if (this->elapsedLabel) this->elapsedLabel->setText("00:00");
    if (this->durationLabel) this->durationLabel->setText("--:--");
    if (this->metaLabel) this->metaLabel->setText("");
    if (this->stage) {
        this->stage->setIndex(index);
        this->stage->setGlyph(firstGlyph(describeAudio(this->file.name).title));
        this->stage->setProgress(0, 0);
    }
    if (this->isAudio) {
        AudioName info = describeAudio(this->file.name);
        MusicSession::instance().setTrack(info.title, info.artist,
                                          this->provider, this->playlist,
                                          index);
    }
    this->setStatus("cloud/player/loading"_i18n);
    // Keep the bottom panel hidden until the viewer asks for it: showing it
    // here made it flash on every entry.
    if (this->osdPanel) this->osdPanel->setVisibility(brls::Visibility::GONE);

    this->play();
    return true;
}

bool PlayerPage::playNext() {
    if (this->shuffle) {
        // Loop one is handled before this call; otherwise a shuffle round
        // restarts only in "loop all", so the queue stops after every track
        // played once when looping is off.
        const size_t next =
            this->nextShuffled(this->loopMode == LoopMode::All);
        if (next >= this->playlist.size()) return false;
        return this->playAt(next);
    }
    return this->playAt(this->playlistIndex + 1);
}

size_t PlayerPage::nextShuffled(bool wrap) {
    if (this->playlist.size() <= 1) return this->playlist.size();
    if (this->shuffleBag.empty()) {
        // A finished round only restarts when wrapping is allowed.
        if (this->shuffleRoundUsed && !wrap) return this->playlist.size();
        this->shuffleBag.reserve(this->playlist.size() - 1);
        for (size_t i = 0; i < this->playlist.size(); i++) {
            if (i != this->playlistIndex) this->shuffleBag.push_back(i);
        }
        std::shuffle(this->shuffleBag.begin(), this->shuffleBag.end(),
                     audioRng());
        this->shuffleRoundUsed = true;
    }
    const size_t next = this->shuffleBag.back();
    this->shuffleBag.pop_back();
    return next;
}

void PlayerPage::moveSelection(int delta) {
    if (this->playlist.empty()) return;
    int next = (int)this->selectedIndex + delta;
    if (next < 0) next = 0;
    if (next >= (int)this->playlist.size())
        next = (int)this->playlist.size() - 1;
    this->selectedIndex = (size_t)next;
    if (this->stage) this->stage->setSelection(this->selectedIndex);
}

void PlayerPage::toggleShuffle() {
    this->shuffle = !this->shuffle;
    this->shuffleBag.clear();
    this->shuffleRoundUsed = false;
    if (this->stage) this->stage->setModes(this->shuffle, (int)this->loopMode);
    this->showPlayerMessage(this->shuffle
                                ? "cloud/player/shuffle_on"_i18n
                                : "cloud/player/shuffle_off"_i18n);
}

void PlayerPage::playNextManual() {
    if (this->playlist.empty()) return;
    if (this->shuffle) {
        const size_t next = this->nextShuffled(true);
        if (next < this->playlist.size()) this->playAt(next);
        return;
    }
    if (this->playlistIndex + 1 < this->playlist.size()) {
        this->playAt(this->playlistIndex + 1);
        return;
    }
    if (this->loopMode == LoopMode::All && this->playlist.size() > 1) {
        this->playAt(0);
        return;
    }
    brls::Application::notify("cloud/player/last"_i18n);
}

void PlayerPage::playPreviousManual() {
    if (this->playlist.empty()) return;
    if (this->playlistIndex > 0) {
        this->playAt(this->playlistIndex - 1);
        return;
    }
    if (this->loopMode == LoopMode::All && this->playlist.size() > 1) {
        this->playAt(this->playlist.size() - 1);
        return;
    }
    brls::Application::notify("cloud/player/first"_i18n);
}

std::string PlayerPage::loopLabel() const {
    switch (this->loopMode) {
        case LoopMode::One:
            return "cloud/player/loop_one"_i18n;
        case LoopMode::All:
            return "cloud/player/loop_all"_i18n;
        default:
            return "cloud/player/loop_off"_i18n;
    }
}

void PlayerPage::toggleLoopMode() {
    switch (this->loopMode) {
        case LoopMode::Off:
            this->loopMode = LoopMode::One;
            break;
        case LoopMode::One:
            this->loopMode = LoopMode::All;
            break;
        default:
            this->loopMode = LoopMode::Off;
            break;
    }
    brls::Application::notify(this->loopLabel());
    if (this->stage) this->stage->setModes(this->shuffle, (int)this->loopMode);
    this->showOsd();
    this->updateOsd();
}

void PlayerPage::showPlaylistMenu() {
    this->showOsd();
    if (this->playlist.size() <= 1) {
        brls::Application::notify("cloud/player/playlist_empty"_i18n);
        return;
    }
    std::vector<std::string> names;
    names.reserve(this->playlist.size());
    for (size_t i = 0; i < this->playlist.size(); i++) {
        names.push_back((i == this->playlistIndex ? "▶ " : "   ") +
                        this->playlist[i].name);
    }
    brls::Dropdown* dropdown = new brls::Dropdown(
        "cloud/player/playlist"_i18n, names,
        [this, alive = this->alive](int selected) {
            if (!*alive) return;
            if (selected < 0 || selected >= (int)this->playlist.size() ||
                (size_t)selected == this->playlistIndex)
                return;
            this->playAt((size_t)selected);
        },
        (int)this->playlistIndex);
    brls::Application::pushActivity(new brls::Activity(dropdown));
}

void PlayerPage::startUrl(const std::string& url, const std::string& headers) {
#ifdef NXKIT_WITH_MPV
    logDebug("player: startUrl '" + this->file.name + "' hdr=" +
             std::to_string(headers.size()) +
             " m3u8=" + (url.find(".m3u8") != std::string::npos ? "1" : "0"));
    this->lastUrl = url;
    this->lastHeaders = headers;
    this->triedSoftwareDec = false;
    this->loadedOnce = false;
    this->probedUrl = false;
    this->triedHttpFallback = false;
    // Stops and resets mpv (so a previous file's headers cannot leak), then
    // applies the provider headers and starts the url.
    startMpvStream(url, headers);
    // A reload (quality change, software decoding fallback) drops the subtitle
    // track too; queue the local copy so it comes back with the video.
    if (!this->activeSubtitlePath.empty())
        this->pendingSubtitlePath = this->activeSubtitlePath;
#else
    (void)url;
    (void)headers;
#endif
}

void PlayerPage::cacheAndPlay() {
#ifdef NXKIT_WITH_MPV
    if (this->lastUrl.empty()) return;
    logDebug("player: caching '" + this->file.name + "' for playback");
    const std::string url = this->lastUrl;
    // Bounded ranges are served to the netdisk identity only.
    const std::string headers = this->fallbackHeaders.empty()
                                    ? this->lastHeaders
                                    : this->fallbackHeaders;
    const std::string name = this->file.name;
    const uint64_t expected = this->file.size;
    const auto alive = this->alive;
    this->setStatus("cloud/player/loading"_i18n);
    nxkit::ensureDirectory(nxkit::dataPath("cache/"));
    cpr::async([this, alive, url, headers, name, expected] {
        std::string stem = name;
        for (char& c : stem) {
            bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                        c == '_';
            if (!keep) c = '_';
        }
        if (stem.size() > 64) stem = stem.substr(stem.size() - 64);
        const std::string path = nxkit::dataPath("cache/" + stem);
        FILE* out = fopen(path.c_str(), "wb");
        if (!out) {
            brls::sync([this, alive] {
                if (*alive) this->setStatus("cloud/player/error"_i18n);
            });
            return;
        }
        constexpr size_t kChunk = 4 * 1024 * 1024;
        uint64_t written = 0;
        uint64_t total = 0;
        bool failed = false;
        int reported = 0;
        while (true) {
            nxkit::HttpClient client;
            client.setHeaderBlock(headers);
            client.setTimeout(60000);
            client.setHeader("Range",
                             "bytes=" + std::to_string(written) + "-" +
                                 std::to_string(written + kChunk - 1));
            nxkit::HttpResult result;
            try {
                result = client.get(url);
            } catch (const std::exception&) {
                failed = true;
                break;
            }
            if (!result.ok || result.body.empty()) {
                failed = true;
                break;
            }
            if (total == 0) {
                auto it = result.headers.find("content-range");
                if (it != result.headers.end()) {
                    size_t slash = it->second.find('/');
                    if (slash != std::string::npos) {
                        try {
                            total = std::stoull(it->second.substr(slash + 1));
                        } catch (...) {
                        }
                    }
                }
            }
            if (fwrite(result.body.data(), 1, result.body.size(), out) !=
                result.body.size()) {
                failed = true;
                break;
            }
            written += result.body.size();
            const uint64_t goal = total > 0 ? total : expected;
            if (goal > 0) {
                int pct = (int)(written * 100 / goal);
                if (pct / 10 != reported / 10) {
                    reported = pct;
                    brls::sync([this, alive, pct] {
                        if (!*alive) return;
                        this->setStatus(brls::getStr("cloud/player/loading") +
                                        " " + std::to_string(pct) + "%");
                    });
                }
            }
            // 200 means the server ignored the range and sent the rest.
            if (result.status != 206) break;
            if (total > 0 && written >= total) break;
            if (result.body.size() < kChunk) break;
        }
        fclose(out);
        if (failed || written == 0) {
            remove(path.c_str());
            brls::sync([this, alive] {
                if (!*alive) return;
                this->setStatus("cloud/player/error"_i18n);
            });
            return;
        }
        logDebug("player: cached " + std::to_string(written) + " bytes");
        brls::sync([this, alive, path] {
            if (!*alive) return;
            this->startUrl(path, "");
            // Keep the flag set: a broken cache must not restart the download.
            this->triedLocalCache = true;
        });
    });
#endif
}

/// Some Quark files use codecs the console's hwdec path rejects; retrying the
/// same url with software decoding usually still plays them.
void PlayerPage::retryWithSoftwareDec() {
#ifdef NXKIT_WITH_MPV
    if (this->triedSoftwareDec || !this->loadedOnce || this->lastUrl.empty())
        return;
    this->triedSoftwareDec = true;
    logDebug("player: retrying with software decoding");
    auto& mpv = nxkit::MPVCore::instance();
    mpv.stop();
    mpv.command_async("set", "hwdec", "no");
    mpv.setUrl(this->lastUrl);
#endif
}

void PlayerPage::onMpvEvent(int event) {
    logDebug("player: mpv event " + std::to_string(event));
#ifdef NXKIT_WITH_MPV
    using nxkit::MpvEventEnum;
    switch ((MpvEventEnum)event) {
        case MpvEventEnum::LOADING_START:
            this->setStatus("cloud/player/loading"_i18n);
            break;
        case MpvEventEnum::LOADING_END:
            this->setStatus("");
            break;
        case MpvEventEnum::MPV_LOADED: {
            this->setStatus("");
            this->loadedOnce = true;
            auto& mpv = nxkit::MPVCore::instance();
            // The demuxer knows the container's audio / subtitle tracks now.
            this->scanTracks();
            // reset() clears mpv properties between files: restore the speed
            // the user picked with the L3 menu.
            if (this->playbackSpeed != 1.0) mpv.setSpeed(this->playbackSpeed);
            // Aspect override is also lost on reset(); restore the user's pick.
            if (this->aspectIndex != 0) this->applyAspect(this->aspectIndex, false);
            // Attach the subtitle that was picked before the file was ready.
            if (!this->pendingSubtitlePath.empty()) {
                std::string path = this->pendingSubtitlePath;
                this->pendingSubtitlePath.clear();
                mpv.command_async("sub-add", path, "select");
                mpv.command_async("set", "sub-visibility", "yes");
            }
            // The stream is seekable now: jump to the remembered position.
            if (this->resumePosition > 0) {
                int64_t target = (int64_t)this->resumePosition;
                this->resumePosition = 0;
                logDebug("player: resume at " + std::to_string(target));
                mpv.seek(target);
                mpv.showOsdText("cloud/player/resume"_i18n + " " +
                                wiliwili::sec2Time(target));
            }
            // Show the OSD once, then let it fade out during playback.
            this->updateOsd();
            break;
        }
        case MpvEventEnum::MPV_FILE_ERROR:
            // Baidu links signed for the web session are sometimes refused
            // when fetched with the browser identity; retry once with the
            // download headers (netdisk UA + cookie) before giving up.
            if (!this->triedFallbackHeaders && !this->fallbackHeaders.empty() &&
                !this->lastUrl.empty()) {
                this->triedFallbackHeaders = true;
                logDebug("player: retrying with provider download headers");
                this->setStatus("cloud/player/loading"_i18n);
                this->startUrl(this->lastUrl, this->fallbackHeaders);
                break;
            }
            // ffmpeg's mbedtls read hits EOF on the Baidu CDN while the same
            // link answers fine over plain http, so retry it downgraded once.
            if (!this->triedHttpFallback && !this->lastUrl.empty() &&
                this->lastUrl.rfind("https://", 0) == 0 &&
                this->lastUrl.find("baidupcs.com") != std::string::npos) {
                this->triedHttpFallback = true;
                std::string httpUrl = "http://" + this->lastUrl.substr(8);
                std::string httpHeaders =
                    this->provider ? this->provider->playbackHeaders()
                                   : this->lastHeaders;
                if (httpHeaders.empty()) httpHeaders = this->lastHeaders;
                logDebug("player: retrying baidu link over http");
                this->setStatus("cloud/player/loading"_i18n);
                this->startUrl(httpUrl, httpHeaders);
                break;
            }
            // Some Baidu files only answer bounded (<= 4 MB) range requests,
            // which the demuxer cannot use; audio is small enough to pull into
            // the local cache once and play from there.
            if (this->isAudio && !this->triedLocalCache && this->provider &&
                this->provider->id() == "baidu" && !this->lastUrl.empty() &&
                this->lastUrl.find("://") != std::string::npos) {
                this->triedLocalCache = true;
                this->cacheAndPlay();
                break;
            }
            this->setStatus("cloud/player/error"_i18n);
            this->showOsd(false);
            if (!this->probedUrl && !this->lastUrl.empty() &&
                this->lastUrl.rfind("http", 0) == 0) {
                this->probedUrl = true;
                const std::string url = this->lastUrl;
                const std::string headers = this->lastHeaders;
                const std::string altHeaders =
                    this->provider ? this->provider->playbackHeaders()
                                   : std::string();
                cpr::async([this, alive = this->alive, url, headers,
                            altHeaders] {
                    auto fetch = [&url](const std::string& fields) {
                        nxkit::HttpResult out;
                        try {
                            nxkit::HttpClient client;
                            // Only the status matters; do not pull the film.
                            client.setHeaderBlock(fields +
                                                  "Range: bytes=0-1023\r\n");
                            client.setTimeout(20000);
                            out = client.get(url);
                        } catch (const std::exception& e) {
                            out.ok = false;
                            out.error = e.what();
                        }
                        return out;
                    };
                    nxkit::HttpResult result = fetch(headers);
                    logDebug("player: probe status=" +
                             std::to_string(result.status) +
                             (result.error.empty()
                                  ? " body=" + result.body.substr(0, 120)
                                  : " error=" + result.error));
                    if (!altHeaders.empty() && altHeaders != headers) {
                        nxkit::HttpResult alt = fetch(altHeaders);
                        logDebug("player: probe playback status=" +
                                 std::to_string(alt.status) +
                                 (alt.error.empty()
                                      ? " body=" + alt.body.substr(0, 120)
                                      : " error=" + alt.error));
                        if (alt.status >= 200 && alt.status < 300)
                            result = alt;
                    }
                    if (result.status < 400) return;
                    const std::string text =
                        "cloud/player/error"_i18n + "  HTTP " +
                        std::to_string(result.status);
                    brls::sync([this, alive, text] {
                        if (!*alive) return;
                        this->setStatus(text);
                    });
                });
            }
            this->retryWithSoftwareDec();
            break;
        case MpvEventEnum::END_OF_FILE:
            // The video ran to the end: nothing to resume next time.
            PlaybackStore::instance().update(this->file.id, 0, 0);
            if (this->loopMode == LoopMode::One) {
                this->playAt(this->playlistIndex);
                return;
            }
            // Play the next file of the same directory, if there is one.
            if (this->playNext()) return;
            if (this->loopMode == LoopMode::All) {
                this->playAt(0);
                return;
            }
            logDebug("player: end of file, back to list");
            brls::Application::popActivity();
            return;
        case MpvEventEnum::UPDATE_PROGRESS:
            break;
        default:
            break;
    }
    this->updateOsd();
#endif
}

void PlayerPage::setStatus(const std::string& text) {
    this->statusOverride = !text.empty();
    // Video: the status replaces the resolution line in the OSD; audio: it
    // becomes the state line under the record.
    if (this->metaLabel) {
        this->metaLabel->setText(text);
        return;
    }
    if (this->stage) this->stage->setStatus(text);
}

void PlayerPage::updateOsd() {
#ifdef NXKIT_WITH_MPV
    auto& mpv = nxkit::MPVCore::instance();

    if (this->isAudio) {
        if (!this->stage) return;
        const double duration = mpv.duration;
        const bool running = !mpv.isPaused() && !mpv.isStopped();
        this->stage->setPlaying(running);
        this->stage->setProgress(mpv.playback_time, duration);
        MusicSession::instance().setPlaying(running);

        // A loading / error line set through setStatus wins until it is
        // cleared; otherwise the stage shows the loop / speed / paused state.
        if (this->statusOverride) return;
        std::string text;
        if (this->loopMode != LoopMode::Off) text = this->loopLabel();
        if (this->playbackSpeed != 1.0) {
            if (!text.empty()) text += "  ·  ";
            text += speedText(this->playbackSpeed);
        }
        if (mpv.isPaused()) {
            if (!text.empty()) text += "  ·  ";
            text += "cloud/player/paused"_i18n;
        }
        this->stage->setStatus(text);
        return;
    }

    if (mpv.isStopped()) return;

    const double duration = mpv.duration;
    if (this->seekBar)
        this->seekBar->setProgress(duration > 0
                                       ? (float)(mpv.playback_time / duration)
                                       : 0.0f);
    if (this->elapsedLabel)
        this->elapsedLabel->setText(
            wiliwili::sec2Time((int64_t)mpv.playback_time));
    if (this->durationLabel)
        this->durationLabel->setText(
            duration > 0 ? wiliwili::sec2Time((int64_t)duration) : "--:--");

    if (!this->metaLabel || this->statusOverride) return;
    std::string meta;
    if (this->playlist.size() > 1)
        meta = std::to_string(this->playlistIndex + 1) + "/" +
               std::to_string(this->playlist.size());
    if (this->streamIndex >= 0 && this->streamIndex < (int)this->streams.size()) {
        if (!meta.empty()) meta += "  ·  ";
        meta += this->streams[this->streamIndex].resolution;
    }
    if (this->loopMode != LoopMode::Off) {
        if (!meta.empty()) meta += "  ·  ";
        meta += this->loopLabel();
    }
    if (this->playbackSpeed != 1.0) {
        if (!meta.empty()) meta += "  ·  ";
        meta += speedText(this->playbackSpeed);
    }
    if (mpv.isPaused()) {
        if (!meta.empty()) meta += "  ·  ";
        meta += "cloud/player/paused"_i18n;
    }
    this->metaLabel->setText(meta);
#endif
}

void PlayerPage::seekBy(int64_t seconds) {
#ifdef NXKIT_WITH_MPV
    auto& mpv = nxkit::MPVCore::instance();
    if (mpv.isStopped()) return;
    mpv.seekRelative(seconds);
    // Show the target position right away: on a remote stream the next
    // playback-time update can take a moment to arrive.
    double target = mpv.playback_time + (double)seconds;
    if (target < 0) target = 0;
    if (mpv.duration > 0 && target > mpv.duration) target = mpv.duration;

    if (this->seekBar && mpv.duration > 0)
        this->seekBar->setProgress((float)(target / mpv.duration));
    if (this->stage && mpv.duration > 0)
        this->stage->setProgress(target, mpv.duration);
    if (this->elapsedLabel)
        this->elapsedLabel->setText(wiliwili::sec2Time((int64_t)target));
    if (this->durationLabel)
        this->durationLabel->setText(
            mpv.duration > 0 ? wiliwili::sec2Time((int64_t)mpv.duration) : "--:--");
    if (this->metaLabel)
        this->metaLabel->setText(seconds > 0 ? "cloud/player/forward"_i18n
                                             : "cloud/player/backward"_i18n);
    if (this->stage)
        this->showPlayerMessage(seconds > 0 ? "cloud/player/forward"_i18n
                                           : "cloud/player/backward"_i18n);
    this->showOsd();
#else
    (void)seconds;
#endif
}

void PlayerPage::adjustVolume(int delta) {
#ifdef NXKIT_WITH_MPV
    auto& mpv = nxkit::MPVCore::instance();
    if (mpv.isStopped()) return;
    int shown = -1;
#ifdef __SWITCH__
    // Prefer the console's own volume, so the OSD number matches the system
    // slider and the change is audible outside the player too.
    shown = SystemVolumeControl::instance().step(delta);
#endif
    if (shown < 0) {
        // Fallback: scale mpv only. VIDEO_VOLUME is updated synchronously by
        // setVolume, unlike the observed property, so fast presses still step.
        int value = nxkit::MPVCore::VIDEO_VOLUME + delta;
        if (value < 0) value = 0;
        if (value > 100) value = 100;
        mpv.setVolume(value);
        shown = value;
    }
    if (this->isAudio)
        this->showPlayerMessage("cloud/player/volume"_i18n + " " +
                                std::to_string(shown));
    else
        this->showVolume(shown);
#else
    (void)delta;
#endif
}

void PlayerPage::showVolume(int percent) {
    this->volumePercent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    this->volumeVisible = true;
    if (this->volumeTimer) brls::cancelDelay(this->volumeTimer);
    this->volumeTimer = brls::delay(1400, [this, alive = this->alive] {
        if (!*alive) return;
        this->volumeTimer = 0;
        this->volumeVisible = false;
    });
}

/// Vertical meter on the left edge, vertically centred, so the volume change is
/// readable without covering the middle of the picture.
void PlayerPage::drawVolume(NVGcontext* vg, float x, float y, float width,
                            float height) {
    if (this->isAudio || !this->volumeVisible || this->volumePercent < 0)
        return;

    const float barWidth = 8.0f;
    const float barHeight = 190.0f;
    const float bx = x + 34.0f;
    const float by = y + (height - barHeight) * 0.5f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, bx, by, barWidth, barHeight, barWidth * 0.5f);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 54));
    nvgFill(vg);

    const float level = barHeight * (float)this->volumePercent / 100.0f;
    if (level > 0.5f) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, bx, by + barHeight - level, barWidth, level,
                       barWidth * 0.5f);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 235));
        nvgFill(vg);
    }

    nvgFontFaceId(vg, brls::Application::getDefaultFont());
    nvgFontSize(vg, 20.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 225));
    const std::string value = std::to_string(this->volumePercent);
    nvgText(vg, bx + barWidth + 12.0f, by + barHeight * 0.5f, value.c_str(),
            nullptr);
}

void PlayerPage::showOsd(bool autoHide) {
    if (this->osdTimer) {
        brls::cancelDelay(this->osdTimer);
        this->osdTimer = 0;
    }
    if (this->isAudio || !this->osdPanel) return;

    this->osdPanel->setVisibility(brls::Visibility::VISIBLE);
    if (!autoHide) return;

    // Hide again after a few seconds of uninterrupted playback.
    this->osdTimer = brls::delay(4200, [this, alive = this->alive] {
        if (!*alive) return;
        this->osdTimer = 0;
#ifdef NXKIT_WITH_MPV
        auto& mpv = nxkit::MPVCore::instance();
        if (mpv.isStopped() || mpv.isPaused()) return;
#endif
        if (this->osdPanel)
            this->osdPanel->setVisibility(brls::Visibility::GONE);
    });
}

void PlayerPage::showPlayerMessage(const std::string& text) {
    const int token = ++this->messageToken;
    // The audio stage has no hint label: transient feedback ("音量 60") takes
    // over its state line and falls back to the loop / paused state after a
    // moment.
    if (this->stage) {
        this->setStatus(text);
        brls::delay(3000, [this, alive = this->alive, token] {
            if (!*alive || token != this->messageToken) return;
            this->setStatus("");
            this->updateOsd();
        });
        return;
    }
    if (!this->hintLabel) return;
    const std::string hint = "cloud/player/hint_video"_i18n;
    this->hintLabel->setText(text);
    this->showOsd();
    brls::delay(3000, [this, alive = this->alive, hint, token] {
        if (!*alive || token != this->messageToken) return;
        if (this->hintLabel) this->hintLabel->setText(hint);
    });
}

void PlayerPage::showQualityMenu() {
#ifdef NXKIT_WITH_MPV
    logDebug("player: Y pressed, streams=" + std::to_string(this->streams.size()));
    if (this->streams.empty()) {
        // The transcode request failed earlier (the CDN sometimes answers 400)
        // and playback fell back to the raw download link. Ask again on demand
        // instead of silently ignoring the key.
        this->provider->videoStreams(
            this->file.id,
            [this, alive = this->alive](const std::vector<VideoStream>& streams,
                                        const std::string& def,
                                        const std::string& error) {
                brls::sync([this, alive, streams, def, error] {
                    if (!*alive) return;
                    logDebug("player: quality refetch n=" +
                             std::to_string(streams.size()) + " err=" + error);
                    if (streams.empty() || !error.empty()) {
                        brls::Application::notify(
                            "cloud/player/no_quality"_i18n);
                        return;
                    }
                    this->streams = streams;
                    this->streamIndex = 0;
                    for (size_t i = 0; i < streams.size(); i++) {
                        if (streams[i].resolution == def) {
                            this->streamIndex = (int)i;
                            break;
                        }
                    }
                    this->presentQualityMenu();
                });
            });
        return;
    }
    this->presentQualityMenu();
#endif
}

void PlayerPage::presentQualityMenu() {
#ifdef NXKIT_WITH_MPV
    std::vector<std::string> names;
    for (const auto& s : this->streams) {
        // Transcoded playlists only carry a quality name, original files
        // carry a resolution too.
        if (s.width > 0 && s.height > 0)
            names.push_back(s.resolution + "  " + std::to_string(s.width) +
                            "x" + std::to_string(s.height));
        else
            names.push_back(s.resolution);
    }
    // A Dropdown is a full screen view, it only becomes visible once pushed as
    // an activity (see the borealis demo).
    brls::Dropdown* dropdown = new brls::Dropdown(
        "cloud/player/quality"_i18n, names,
        [this](int selected) {
            if (selected < 0 || selected >= (int)this->streams.size() ||
                selected == this->streamIndex)
                return;
            this->streamIndex = selected;
            this->startUrl(this->streams[selected].url,
                           this->provider->playbackHeaders());
        },
        this->streamIndex);
    brls::Application::pushActivity(new brls::Activity(dropdown));
#endif
}

void PlayerPage::showSpeedMenu() {
#ifdef NXKIT_WITH_MPV
    static const double kSpeeds[] = {0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0};
    const int count = (int)(sizeof(kSpeeds) / sizeof(kSpeeds[0]));

    std::vector<std::string> items;
    int current = 2;
    for (int i = 0; i < count; i++) {
        items.push_back(speedText(kSpeeds[i]));
        if (std::fabs(kSpeeds[i] - this->playbackSpeed) < 1e-6) current = i;
    }

    brls::Dropdown* dropdown = new brls::Dropdown(
        "cloud/player/speed"_i18n, items,
        [this](int selected) {
            if (selected < 0 || selected >= count) return;
            this->applySpeed(kSpeeds[selected]);
        },
        current);
    brls::Application::pushActivity(new brls::Activity(dropdown));
#endif
}

void PlayerPage::applySpeed(double speed) {
#ifdef NXKIT_WITH_MPV
    this->playbackSpeed = speed;
    nxkit::MPVCore::instance().setSpeed(speed);
#endif
    this->showOsd();
    this->showPlayerMessage(brls::getStr("cloud/player/speed_on", speedText(speed)));
    this->updateOsd();
}

void PlayerPage::showAspectMenu() {
#ifdef NXKIT_WITH_MPV
    std::vector<std::string> items;
    for (int i = 0; i < kAspectCount; i++)
        items.push_back(aspectLabel(kAspects[i]));

    brls::Dropdown* dropdown = new brls::Dropdown(
        "cloud/player/aspect"_i18n, items,
        [this, alive = this->alive](int selected) {
            if (!*alive) return;
            this->applyAspect(selected, true);
        },
        this->aspectIndex);
    brls::Application::pushActivity(new brls::Activity(dropdown));
#endif
}

void PlayerPage::applyAspect(int index, bool notify) {
    if (index < 0 || index >= kAspectCount) return;
    this->aspectIndex = index;
#ifdef NXKIT_WITH_MPV
    auto& mpv = nxkit::MPVCore::instance();
    const AspectOption& option = kAspects[index];
    if (option.value) {
        mpv.command_async("set", "keepaspect", "yes");
        mpv.command_async("set", "video-aspect-override", option.value);
    } else {
        // "Stretch": ignore the source ratio and fill the screen.
        mpv.command_async("set", "keepaspect", "no");
        mpv.command_async("set", "video-aspect-override", "no");
    }
#endif
    if (!notify) return;
    this->showOsd();
    this->showPlayerMessage(aspectLabel(kAspects[index]));
    this->updateOsd();
}

void PlayerPage::requestSubtitles() {
    this->subtitles.clear();
    this->subtitleIndex = -1;
    this->activeSubtitlePath.clear();
    this->subtitleScanId = this->file.id;
    this->subtitleScanStem = stemOf(this->file.name);
    this->subtitleScanPage = 0;
    if (!this->provider || this->file.parentId.empty()) {
        logDebug("player: subtitle scan skipped (no parent dir)");
        return;
    }
    this->fetchSubtitlePage();
}

void PlayerPage::fetchSubtitlePage() {
    const std::string fileId = this->subtitleScanId;
    const std::string parentId = this->file.parentId;
    const std::string stem = this->subtitleScanStem;
    const int page = ++this->subtitleScanPage;
    logDebug("player: subtitle scan dir=" + parentId + " page=" +
             std::to_string(page));
    this->provider->listFiles(
        parentId, page, 100,
        [this, alive = this->alive, fileId, stem, page](
            const std::vector<DriveFile>& files, int total,
            const std::string& error) {
            brls::sync([this, alive, fileId, stem, page, files, total, error] {
                if (!*alive) return;
                if (this->subtitleScanId != fileId) return;
                if (!error.empty()) {
                    logDebug("player: subtitle scan err=" + error);
                    return;
                }

                int matched = -2;  // -2: nothing yet
                for (const auto& file : files) {
                    if (file.isDir || !isSubtitleName(file.name)) continue;
                    this->subtitles.push_back(file);
                    if (matched == -2 && subtitleMatchesVideo(stem, file.name))
                        matched = (int)this->subtitles.size() - 1;
                }

                // Big folders paginate; two extra pages cover a whole season
                // of episodes with their subtitle files.
                bool more =
                    !files.empty() && page < 3 &&
                    (total < 0 ? files.size() >= 100 : page * 100 < total);
                if (more) {
                    this->fetchSubtitlePage();
                    return;
                }

                logDebug("player: subtitles n=" +
                         std::to_string(this->subtitles.size()));
                if (this->subtitles.empty()) return;
                // Auto-load the same-name subtitle, or the only one around.
                if (matched == -2 && this->subtitles.size() == 1) matched = 0;
                if (matched >= 0) this->selectSubtitle(matched);
            });
        });
}

/// Human readable name of an embedded track: its title, its language or a
/// plain fallback when the container carries neither.
std::string PlayerPage::trackText(const MediaTrack& track) {
    std::string text = track.title;
    if (!track.lang.empty() && track.lang != text)
        text = text.empty() ? track.lang : text + " (" + track.lang + ")";
    if (text.empty()) text = "Track " + std::to_string(track.id);
    if (!track.codec.empty()) text += " · " + track.codec;
    return text;
}

void PlayerPage::scanTracks() {
#ifdef NXKIT_WITH_MPV
    auto& mpv = nxkit::MPVCore::instance();
    this->audioTracks.clear();
    this->embeddedSubtitles.clear();
    if (mpv.isStopped()) return;

    // aid / sid hold the id of the active track, "no" when disabled. Track
    // entries themselves are read through track-list/<n>/... properties; format
    // flags are requested as strings because mpv only converts those to int64
    // reliably for numeric properties.
    auto parseId = [](const std::string& value) -> int64_t {
        return (value.empty() || value == "no") ? -1 : atoll(value.c_str());
    };
    const int64_t audioId = parseId(mpv.getString("aid"));
    const int64_t subId = parseId(mpv.getString("sid"));

    const int64_t count = mpv.getInt("track-list/count");
    for (int64_t i = 0; i < count; i++) {
        const std::string prefix = "track-list/" + std::to_string(i) + "/";
        const std::string type = mpv.getString(prefix + "type");
        if (type != "audio" && type != "sub") continue;
        // External subtitles are the files this page adds itself; they are
        // already listed separately in the menu.
        if (type == "sub" && mpv.getString(prefix + "external") == "yes") continue;

        MediaTrack track;
        track.id = mpv.getInt(prefix + "id");
        track.title = mpv.getString(prefix + "title");
        track.lang = mpv.getString(prefix + "lang");
        track.codec = mpv.getString(prefix + "codec");
        if (type == "audio") {
            track.selected = track.id == audioId;
            this->audioTracks.push_back(std::move(track));
        } else {
            track.selected = track.id == subId;
            this->embeddedSubtitles.push_back(std::move(track));
        }
    }
    logDebug("player: tracks audio=" + std::to_string(this->audioTracks.size()) +
             " sub=" + std::to_string(this->embeddedSubtitles.size()));
#endif
}

void PlayerPage::showTrackMenu() {
#ifdef NXKIT_WITH_MPV
    // Pick up tracks added after load (mpv exposes HLS subtitle groups late).
    this->scanTracks();

    bool subSelected = this->subtitleIndex >= 0;
    for (const auto& track : this->embeddedSubtitles)
        subSelected = subSelected || track.selected;
    if (this->subtitles.empty() && this->embeddedSubtitles.empty() &&
        this->audioTracks.empty()) {
        this->showPlayerMessage("cloud/player/track_none"_i18n);
        return;
    }

    enum class Kind { Off, EmbeddedSub, FileSub, Audio, Header };
    struct Item {
        Kind kind;
        int index;
        /// mpv track id for embedded entries; index would go stale when the
        /// stream reloads while the menu is open.
        int64_t trackId;
    };

    std::vector<std::string> names;
    std::vector<Item> items;
    // The menu starts with "subtitles off" so a mis-downloaded track can be
    // hidden without leaving the video.
    names.push_back(std::string(subSelected ? "   " : "▶ ") +
                    "cloud/player/subtitle_off"_i18n);
    items.push_back({Kind::Off, -1, 0});

    for (size_t i = 0; i < this->embeddedSubtitles.size(); i++) {
        const MediaTrack& track = this->embeddedSubtitles[i];
        names.push_back(std::string(track.selected ? "▶ " : "   ") +
                        brls::getStr("cloud/player/track_embedded") + " · " +
                        trackText(track));
        items.push_back({Kind::EmbeddedSub, (int)i, track.id});
    }
    for (size_t i = 0; i < this->subtitles.size(); i++) {
        names.push_back(std::string((int)i == this->subtitleIndex ? "▶ " : "   ") +
                        brls::getStr("cloud/player/track_file") + " · " +
                        this->subtitles[i].name);
        items.push_back({Kind::FileSub, (int)i, 0});
    }
    if (!this->audioTracks.empty()) {
        names.push_back("── " + brls::getStr("cloud/player/track_audio") + " ──");
        items.push_back({Kind::Header, -1, 0});
        for (size_t i = 0; i < this->audioTracks.size(); i++) {
            const MediaTrack& track = this->audioTracks[i];
            names.push_back(std::string(track.selected ? "▶ " : "   ") +
                            trackText(track));
            items.push_back({Kind::Audio, (int)i, track.id});
        }
    }

    // Highlight the currently active entry.
    int current = 0;
    for (size_t i = 0; i < names.size(); i++) {
        if (names[i].compare(0, 4, "▶ ") == 0) {
            current = (int)i;
            break;
        }
    }

    // The Dropdown callback copies the item table, so share it by value.
    auto entries = std::make_shared<std::vector<Item>>(std::move(items));
    brls::Dropdown* dropdown = new brls::Dropdown(
        "cloud/player/track"_i18n, names,
        [this, alive = this->alive, entries](int selected) {
            if (!*alive) return;
            if (selected < 0 || selected >= (int)entries->size()) return;
            const Item& item = (*entries)[selected];
            switch (item.kind) {
                case Kind::Off:
                    this->selectSubtitle(-1);
                    break;
                case Kind::EmbeddedSub:
                    this->selectEmbeddedSubtitle(item.trackId);
                    break;
                case Kind::FileSub:
                    this->selectSubtitle(item.index);
                    break;
                case Kind::Audio:
                    this->selectAudioTrack(item.trackId);
                    break;
                default:
                    break;
            }
        },
        current);
    brls::Application::pushActivity(new brls::Activity(dropdown));
#endif
}

void PlayerPage::selectEmbeddedSubtitle(int64_t trackId) {
#ifdef NXKIT_WITH_MPV
    auto& mpv = nxkit::MPVCore::instance();
    // An embedded track replaces any downloaded subtitle file.
    this->subtitleIndex = -1;
    this->activeSubtitlePath.clear();
    this->pendingSubtitlePath.clear();

    std::string label;
    for (auto& track : this->embeddedSubtitles) {
        track.selected = track.id == trackId;
        if (track.selected) label = trackText(track);
    }
    mpv.command_async("set", "sid",
                      trackId < 0 ? std::string("no") : std::to_string(trackId));
    mpv.command_async("set", "sub-visibility", trackId < 0 ? "no" : "yes");
    this->showOsd();
    this->showPlayerMessage(
        "cloud/player/subtitle_switched"_i18n + (label.empty() ? "" : " " + label));
    this->updateOsd();
#endif
}

void PlayerPage::selectAudioTrack(int64_t trackId) {
#ifdef NXKIT_WITH_MPV
    if (trackId < 0) return;
    auto& mpv = nxkit::MPVCore::instance();
    std::string label;
    for (auto& track : this->audioTracks) {
        track.selected = track.id == trackId;
        if (track.selected) label = trackText(track);
    }
    mpv.command_async("set", "aid", std::to_string(trackId));
    logDebug("player: audio track -> " + std::to_string(trackId));
    this->showOsd();
    this->showPlayerMessage(
        "cloud/player/audio_switched"_i18n + (label.empty() ? "" : " " + label));
    this->updateOsd();
#endif
}

void PlayerPage::selectSubtitle(int index) {
#ifdef NXKIT_WITH_MPV
    auto& mpv = nxkit::MPVCore::instance();
    if (index < 0 || index >= (int)this->subtitles.size()) {
        this->subtitleIndex = -1;
        this->activeSubtitlePath.clear();
        this->pendingSubtitlePath.clear();
        // Also drop an embedded subtitle track that may be selected.
        mpv.command_async("set", "sid", "no");
        mpv.command_async("set", "sub-visibility", "no");
        this->showPlayerMessage("cloud/player/subtitle_off"_i18n);
        return;
    }
    if (!this->provider) return;

    const DriveFile sub = this->subtitles[index];
    const std::string fileId = this->file.id;
    logDebug("player: subtitle load '" + sub.name + "'");
    this->provider->downloadInfo(
        sub.id,
        [this, alive = this->alive, fileId, sub, index](
            const std::string& url, const std::string& headers,
            const std::string& error) {
            brls::sync([this, alive, fileId, sub, index, url, headers, error] {
                if (!*alive) return;
                if (this->file.id != fileId) return;
                if (!error.empty() || url.empty()) {
                    logDebug("player: subtitle url err=" + error);
                    this->showPlayerMessage(
                        "cloud/player/subtitle_failed"_i18n);
                    return;
                }
                const std::string ext = extensionOf(sub.name);
                // Download and re-encode off the main thread, mpv only eats
                // UTF-8 subtitles and Chinese releases are often GBK.
                cpr::async([this, alive, fileId, sub, index, url, headers,
                            ext] {
                    std::string body;
                    std::string reason;
                    try {
                        nxkit::HttpClient client;
                        client.setHeaderBlock(headers);
                        client.setTimeout(30000);
                        nxkit::HttpResult result = client.get(url);
                        if (result.ok) {
                            body = std::move(result.body);
                        } else {
                            reason = "http " + std::to_string(result.status);
                            if (!result.error.empty())
                                reason += " " + result.error;
                        }
                    } catch (const std::exception& e) {
                        reason = std::string("net ") + e.what();
                    }
                    logDebug("player: subtitle dl bytes=" +
                             std::to_string(body.size()) + " reason=" + reason);

                    std::string path;
                    if (!body.empty()) {
                        body = subtitleToUtf8(body);
                        nxkit::ensureDirectory(nxkit::dataPath("subtitles/"));
                        // Baidu uses the full path as the file id, so the id
                        // cannot be dropped into a flat directory verbatim.
                        std::string stem = sub.id;
                        for (char& c : stem) {
                            bool keep = (c >= 'a' && c <= 'z') ||
                                        (c >= 'A' && c <= 'Z') ||
                                        (c >= '0' && c <= '9') || c == '.' ||
                                        c == '-' || c == '_';
                            if (!keep) c = '_';
                        }
                        if (stem.size() > 96) stem = stem.substr(stem.size() - 96);
                        path = nxkit::dataPath("subtitles/" + stem + "." +
                                               (ext.empty() ? "srt" : ext));
                        std::ofstream out(path, std::ios::binary |
                                                    std::ios::trunc);
                        if (out) {
                            out.write(body.data(), (std::streamsize)body.size());
                            if (!out.good()) path.clear();
                        } else {
                            path.clear();
                        }
                        if (path.empty()) reason = "write";
                    } else if (reason.empty()) {
                        reason = "empty body";
                    }

                    brls::sync([this, alive, fileId, sub, index, path,
                                reason] {
                        if (!*alive) return;
                        if (this->file.id != fileId) return;
                        if (path.empty()) {
                            logDebug("player: subtitle save failed: " + reason);
                            this->showPlayerMessage(
                                "cloud/player/subtitle_failed"_i18n);
                            return;
                        }
                        auto& mpv = nxkit::MPVCore::instance();
                        this->subtitleIndex = index;
                        this->activeSubtitlePath = path;
                        if (mpv.isStopped()) {
                            this->pendingSubtitlePath = path;
                            this->showPlayerMessage(
                                "cloud/player/subtitle_loaded"_i18n + " " +
                                sub.name);
                            return;
                        }
                        const int64_t before = mpv.getInt("track-list/count");
                        mpv.command_async("sub-add", path, "select");
                        mpv.command_async("set", "sub-visibility", "yes");
                        this->showOsd();
                        // mpv adds tracks asynchronously: check that the file
                        // really landed instead of failing silently.
                        brls::delay(1500, [this, alive, fileId, before, sub] {
                            if (!*alive) return;
                            if (this->file.id != fileId) return;
                            auto& mpv = nxkit::MPVCore::instance();
                            int64_t tracks = mpv.getInt("track-list/count");
                            logDebug("player: subtitle tracks=" +
                                     std::to_string(tracks) + " before=" +
                                     std::to_string(before));
                            if (tracks > before)
                                this->showPlayerMessage(
                                    "cloud/player/subtitle_loaded"_i18n + " " +
                                    sub.name);
                            else
                                this->showPlayerMessage(
                                    "cloud/player/subtitle_failed"_i18n);
                        });
                    });
                });
            });
        });
#endif
}

/// Stores the current playback position so the next open resumes here.
bool PlayerPage::savePosition() {
#ifdef NXKIT_WITH_MPV
    auto& mpv = nxkit::MPVCore::instance();
    if (mpv.isStopped() || this->file.id.empty()) return false;
    logDebug("player: save position " +
             std::to_string((int64_t)mpv.playback_time) + "/" +
             std::to_string((int64_t)mpv.duration));
    PlaybackStore::instance().update(this->file.id, mpv.playback_time,
                                     (double)mpv.duration);
    return true;
#else
    return false;
#endif
}

void PlayerPage::draw(NVGcontext* vg, float x, float y, float width,
                      float height, brls::Style style,
                      brls::FrameContext* ctx) {
    // The audio page paints its own immersive backdrop inside AudioStageView.
#ifdef NXKIT_WITH_MPV
    if (!this->isAudio) {
        auto& mpv = nxkit::MPVCore::instance();
        // Layout coordinates are content (dp) units: hand the whole content
        // area over, not the framebuffer size. Docked mode runs at a 1.5 scale
        // factor, and mixing pixels in here stretched the video quad past the
        // screen so the picture was cropped.
        if (!mpv.isStopped())
            mpv.draw(brls::Rect(0, 0, (float)brls::Application::contentWidth,
                                (float)brls::Application::contentHeight));
        this->drawVolume(vg, x, y, width, height);
    }
#endif
    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

}  // namespace cloud
