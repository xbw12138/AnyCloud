//
// cloud: video / music playback page (libmpv).
//

#include "cloud/pages/player_page.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include <borealis/core/thread.hpp>

#include "cloud/drive/playback_store.hpp"
#include "cloud/drive/recent_store.hpp"
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

}  // namespace

/// Bottom OSD backdrop: fades to black so the white labels stay readable
/// whatever is on screen.
class PlayerOsdPanel : public brls::Box {
public:
    using brls::Box::Box;

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (width > 0 && height > 0) {
            NVGpaint gradient = nvgLinearGradient(
                vg, x, y, x, y + height, nvgRGBA(0, 0, 0, 0),
                nvgRGBA(0, 0, 0, 200));
            nvgBeginPath(vg);
            nvgRect(vg, x, y, width, height);
            nvgFillPaint(vg, gradient);
            nvgFill(vg);
        }
        brls::Box::draw(vg, x, y, width, height, style, ctx);
    }
};

void SeekBarView::draw(NVGcontext* vg, float x, float y, float width,
                       float height, brls::Style style,
                       brls::FrameContext* ctx) {
    if (width <= 0 || height <= 0) return;

    const float centerY = y + height * 0.5f;
    const float trackHeight = 5.0f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, centerY - trackHeight * 0.5f, width, trackHeight,
                   trackHeight * 0.5f);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 70));
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
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 255));
    nvgFill(vg);
}

/// Extracts one header from a CRLF separated provider header block.
static std::string headerValue(const std::string& headers,
                               const std::string& name) {
    size_t start = 0;
    while (start <= headers.size()) {
        size_t end = headers.find('\n', start);
        std::string line = headers.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > name.size() &&
            line.compare(0, name.size(), name) == 0 &&
            line[name.size()] == ':') {
            size_t value = line.find_first_not_of(" \t", name.size() + 1);
            return value == std::string::npos ? "" : line.substr(value);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return "";
}

/// Same block without the given header (mpv gets the user agent through its
/// own property, see startUrl).
static std::string headersWithout(const std::string& headers,
                                  const std::string& name) {
    std::string out;
    size_t start = 0;
    while (start <= headers.size()) {
        size_t end = headers.find('\n', start);
        std::string line = headers.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        std::string trimmed = line;
        if (!trimmed.empty() && trimmed.back() == '\r') trimmed.pop_back();
        bool drop = trimmed.size() > name.size() &&
                    trimmed.compare(0, name.size(), name) == 0 &&
                    trimmed[name.size()] == ':';
        if (!drop) {
            if (!out.empty()) out += "\r\n";
            out += trimmed;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

/// mpv wants extra headers as a single comma separated list, providers return
/// them CRLF separated (as they would appear in a request).
static std::string headersToMpv(const std::string& headers) {
    std::string out;
    out.reserve(headers.size());
    for (size_t i = 0; i < headers.size(); i++) {
        if (headers[i] == '\r') continue;
        if (headers[i] == '\n') {
            if (!out.empty() && out.back() != ',') out += ',';
            continue;
        }
        out += headers[i];
    }
    while (!out.empty() && out.back() == ',') out.pop_back();
    return out;
}

PlayerPage::PlayerPage(std::shared_ptr<DriveProvider> provider,
                       std::vector<DriveFile> playlist, size_t index,
                       bool isAudio)
    : provider(std::move(provider)),
      isAudio(isAudio),
      playlist(std::move(playlist)),
      playlistIndex(index) {
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
        // Centered music layout.
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setJustifyContent(brls::JustifyContent::CENTER);

        this->titleLabel = new brls::Label();
        this->titleLabel->setText(this->file.name);
        this->titleLabel->setFontSize(28);
        this->titleLabel->setMarginBottom(20);
        this->addView(this->titleLabel);

        this->osdProgress = new brls::Label();
        this->osdProgress->setFontSize(20);
        this->osdProgress->setMarginBottom(12);
        this->addView(this->osdProgress);

        this->loadingLabel = new brls::Label();
        this->loadingLabel->setFontSize(18);
        this->loadingLabel->setText("cloud/player/loading"_i18n);
        this->addView(this->loadingLabel);

        this->hintLabel = new brls::Label();
        this->hintLabel->setFontSize(16);
        this->hintLabel->setMarginTop(24);
        this->hintLabel->setText("cloud/player/hint_audio"_i18n);
        this->addView(this->hintLabel);
    } else {
        // No centered overlay at all: the OSD alone carries the status line,
        // title and progress, so nothing can sit on top of the picture.
        this->osdPanel = new PlayerOsdPanel(brls::Axis::COLUMN);
        this->osdPanel->setJustifyContent(brls::JustifyContent::FLEX_END);
        this->osdPanel->setPadding(30, 44, 20, 44);

        this->osdName = new brls::Label();
        this->osdName->setText(this->file.name);
        this->osdName->setFontSize(26);
        this->osdName->setSingleLine(true);
        this->osdName->setTextColor(nvgRGBA(255, 255, 255, 255));
        this->osdPanel->addView(this->osdName);

        this->metaLabel = new brls::Label();
        this->metaLabel->setFontSize(16);
        this->metaLabel->setMarginTop(4);
        this->metaLabel->setSingleLine(true);
        this->metaLabel->setTextColor(nvgRGBA(255, 255, 255, 170));
        this->osdPanel->addView(this->metaLabel);

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

        this->osdPanel->addView(progressRow);

        this->hintLabel = new brls::Label();
        this->hintLabel->setFontSize(14);
        this->hintLabel->setMarginTop(8);
        this->hintLabel->setSingleLine(true);
        this->hintLabel->setTextColor(nvgRGBA(255, 255, 255, 140));
        this->hintLabel->setText("cloud/player/hint_video"_i18n);
        this->osdPanel->addView(this->hintLabel);

        this->osdPanel->setVisibility(brls::Visibility::GONE);
        this->addView(this->osdPanel);
    }

#ifdef NXKIT_WITH_MPV
    // A: pause / resume. Left/Right: seek. B: exit.
    this->registerAction(
        "", brls::BUTTON_A,
        [this](brls::View*) -> bool {
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
            this->seekBy(-60);
            return true;
        },
        true, true);
    this->registerAction(
        "", brls::BUTTON_UP,
        [this](brls::View*) -> bool {
            this->seekBy(60);
            return true;
        },
        true, true);
    this->registerAction(
        "", brls::BUTTON_B,
        [this](brls::View*) -> bool {
            logDebug("player: exit key, back to list");
            // Remember the position before tearing the player down.
            this->savePosition();
            auto& mpv = nxkit::MPVCore::instance();
            mpv.stop();
            mpv.reset();
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
    // ZL / ZR jump half a minute.
    this->registerAction(
        "", brls::BUTTON_LT,
        [this](brls::View*) -> bool {
            this->seekBy(-30);
            return true;
        },
        true, true);
    this->registerAction(
        "", brls::BUTTON_RT,
        [this](brls::View*) -> bool {
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
    this->savePosition();
#ifdef __SWITCH__
    brls::Application::getPlatform()->disableScreenDimming(
        false, "media playback", "cloud");
#endif
#ifdef NXKIT_WITH_MPV
    auto& mpv = nxkit::MPVCore::instance();
    mpv.getEvent()->unsubscribe(this->eventSub);
    mpv.stop();
    mpv.reset();
#endif
}

void PlayerPage::play() {
    if (!this->mpvAvailable) {
        this->setStatus("cloud/player/unavailable"_i18n);
        return;
    }
#ifdef NXKIT_WITH_MPV
    if (this->isAudio) {
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
    if (this->osdProgress) this->osdProgress->setText("");
    if (this->seekBar) this->seekBar->setProgress(0);
    if (this->elapsedLabel) this->elapsedLabel->setText("00:00");
    if (this->durationLabel) this->durationLabel->setText("--:--");
    if (this->metaLabel) this->metaLabel->setText("");
    this->setStatus("cloud/player/loading"_i18n);
    // Keep the bottom panel hidden until the viewer asks for it: showing it
    // here made it flash on every entry.
    if (this->osdPanel) this->osdPanel->setVisibility(brls::Visibility::GONE);

    this->play();
    return true;
}

bool PlayerPage::playNext() {
    return this->playAt(this->playlistIndex + 1);
}

void PlayerPage::playNextManual() {
    if (this->playlist.empty()) return;
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
    auto& mpv = nxkit::MPVCore::instance();
    mpv.stop();
    mpv.reset();
    // Always overwrite, otherwise a previous file's Cookie headers would leak
    // into the next (header-less) stream. Restore hwdec as well, a previous
    // file may have fallen back to software decoding.
    mpv.command_async("set", "hwdec", "auto");
    // ffmpeg would send mpv's own (Lavf/mpv) User-Agent instead of the one the
    // provider signed the link with, which makes some CDNs answer 403.
    std::string userAgent = headerValue(headers, "User-Agent");
    std::string extraHeaders = headersWithout(headers, "User-Agent");
    if (!userAgent.empty()) mpv.command_async("set", "user-agent", userAgent);
    mpv.setHttpHeaders(extraHeaders.empty() ? "" : headersToMpv(extraHeaders));
    mpv.setUrl(url);
    // A reload (quality change, software decoding fallback) drops the subtitle
    // track too; queue the local copy so it comes back with the video.
    if (!this->activeSubtitlePath.empty())
        this->pendingSubtitlePath = this->activeSubtitlePath;
#else
    (void)url;
    (void)headers;
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
                    if (result.status <= 0) return;
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
    // Video: the status replaces the resolution line in the OSD; audio: the
    // centered title label doubles as the status line.
    if (this->metaLabel) {
        this->metaLabel->setText(text);
        return;
    }
    if (this->loadingLabel) this->loadingLabel->setText(text);
}

void PlayerPage::updateOsd() {
#ifdef NXKIT_WITH_MPV
    auto& mpv = nxkit::MPVCore::instance();

    if (this->isAudio) {
        if (!this->osdProgress) return;
        std::string text = wiliwili::sec2Time((int64_t)mpv.playback_time) +
                           " / " + wiliwili::sec2Time(mpv.duration);
        if (this->loopMode != LoopMode::Off)
            text += "  |  " + this->loopLabel();
        if (this->playbackSpeed != 1.0)
            text += "  |  " + speedText(this->playbackSpeed);
        if (mpv.isPaused()) text += "  |  " + "cloud/player/paused"_i18n;
        this->osdProgress->setText(text);
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
    if (this->elapsedLabel)
        this->elapsedLabel->setText(wiliwili::sec2Time((int64_t)target));
    if (this->durationLabel)
        this->durationLabel->setText(
            mpv.duration > 0 ? wiliwili::sec2Time((int64_t)mpv.duration) : "--:--");
    if (this->metaLabel)
        this->metaLabel->setText(seconds > 0 ? "cloud/player/forward"_i18n
                                             : "cloud/player/backward"_i18n);
    this->showOsd();
#else
    (void)seconds;
#endif
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
    if (!this->hintLabel) return;
    const std::string hint = this->isAudio ? "cloud/player/hint_audio"_i18n
                                           : "cloud/player/hint_video"_i18n;
    this->hintLabel->setText(text);
    this->showOsd();
    const int token = ++this->messageToken;
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
                        path = nxkit::dataPath("subtitles/" + sub.id + "." +
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
    }
#endif
    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

}  // namespace cloud
