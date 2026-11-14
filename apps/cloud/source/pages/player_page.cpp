//
// cloud: video / music playback page (libmpv).
//

#include "cloud/pages/player_page.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "cloud/drive/playback_store.hpp"
#include "cloud/drive/recent_store.hpp"
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
    }

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
                this->startUrl(url, headers);
            });
        });
}

bool PlayerPage::playAt(size_t index) {
    if (index >= this->playlist.size()) return false;

    this->playlistIndex = index;
    this->file = this->playlist[index];
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
    this->triedSoftwareDec = false;
    this->statusOverride = false;

    if (this->titleLabel) this->titleLabel->setText(this->file.name);
    if (this->osdName) this->osdName->setText(this->file.name);
    if (this->osdProgress) this->osdProgress->setText("");
    if (this->seekBar) this->seekBar->setProgress(0);
    if (this->elapsedLabel) this->elapsedLabel->setText("00:00");
    if (this->durationLabel) this->durationLabel->setText("--:--");
    if (this->metaLabel) this->metaLabel->setText("");
    this->setStatus("cloud/player/loading"_i18n);
    this->showOsd(false);

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
    auto& mpv = nxkit::MPVCore::instance();
    mpv.stop();
    mpv.reset();
    // Always overwrite, otherwise a previous file's Cookie headers would leak
    // into the next (header-less) stream. Restore hwdec as well, a previous
    // file may have fallen back to software decoding.
    mpv.command_async("set", "hwdec", "auto");
    mpv.setHttpHeaders(headers.empty() ? "" : headersToMpv(headers));
    mpv.setUrl(url);
#else
    (void)url;
    (void)headers;
#endif
}

/// Some Quark files use codecs the console's hwdec path rejects; retrying the
/// same url with software decoding usually still plays them.
void PlayerPage::retryWithSoftwareDec() {
#ifdef NXKIT_WITH_MPV
    if (this->triedSoftwareDec || this->lastUrl.empty()) return;
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
            // The stream is seekable now: jump to the remembered position.
            if (this->resumePosition > 0) {
                auto& mpv = nxkit::MPVCore::instance();
                int64_t target = (int64_t)this->resumePosition;
                this->resumePosition = 0;
                logDebug("player: resume at " + std::to_string(target));
                mpv.seek(target);
                mpv.showOsdText("cloud/player/resume"_i18n + " " +
                                wiliwili::sec2Time(target));
            }
            // Show the OSD once, then let it fade out during playback.
            this->updateOsd();
            this->showOsd(true);
            break;
        }
        case MpvEventEnum::MPV_FILE_ERROR:
            this->setStatus("cloud/player/error"_i18n);
            this->showOsd(false);
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
    for (const auto& s : this->streams)
        names.push_back(s.resolution + "  " +
                        std::to_string(s.width) + "x" +
                        std::to_string(s.height));
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
        if (!mpv.isStopped())
            mpv.draw(brls::Rect(0, 0, (float)brls::Application::windowWidth,
                                (float)brls::Application::windowHeight));
    }
#endif
    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

}  // namespace cloud
