//
// cloud: video / music playback page (libmpv).
//

#include "cloud/pages/player_page.hpp"

#include <cstdio>

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
                       DriveFile file, bool isAudio)
    : provider(std::move(provider)),
      file(std::move(file)),
      isAudio(isAudio) {
    this->setAxis(brls::Axis::COLUMN);
    this->setGrow(1);
    // Take focus (and hide the highlight box) so the file list cell that
    // opened us does not keep drawing its focus ring over the video.
    this->setFocusable(true);
    this->setHideHighlight(true);

#ifdef NXKIT_WITH_MPV
    this->mpvAvailable = true;
#endif

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
        // Fullscreen video + bottom OSD.
        this->osdBox = new brls::Box(brls::Axis::COLUMN);
        this->osdBox->setJustifyContent(brls::JustifyContent::FLEX_END);
        this->osdBox->setGrow(1);
        this->osdBox->setPadding(0, 24, 20, 24);

        this->osdName = new brls::Label();
        this->osdName->setText(this->file.name);
        this->osdName->setFontSize(18);
        this->osdBox->addView(this->osdName);

        brls::Box* row = new brls::Box(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);

        this->osdProgress = new brls::Label();
        this->osdProgress->setGrow(1);
        this->osdProgress->setFontSize(18);
        row->addView(this->osdProgress);

        this->hintLabel = new brls::Label();
        this->hintLabel->setFontSize(16);
        this->hintLabel->setText("cloud/player/hint_video"_i18n);
        row->addView(this->hintLabel);

        this->osdBox->addView(row);
        this->addView(this->osdBox);

        this->loadingLabel = new brls::Label();
        this->loadingLabel->setFontSize(20);
        this->loadingLabel->setText("cloud/player/loading"_i18n);
        this->loadingLabel->setMargins(24, 24, 24, 24);
        this->addView(this->loadingLabel);
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
            auto& mpv = nxkit::MPVCore::instance();
            mpv.stop();
            mpv.reset();
            brls::Application::popActivity();
            return true;
        },
        true);

    if (!this->isAudio) {
        this->registerAction(
            "cloud/player/quality"_i18n, brls::BUTTON_Y,
            [this](brls::View*) -> bool {
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
    *this->alive = false;
#ifdef NXKIT_WITH_MPV
    auto& mpv = nxkit::MPVCore::instance();
    mpv.getEvent()->unsubscribe(this->eventSub);
    mpv.stop();
    mpv.reset();
#endif
}

void PlayerPage::play() {
    if (!this->mpvAvailable) {
        this->loadingLabel->setText("cloud/player/unavailable"_i18n);
        return;
    }
#ifdef NXKIT_WITH_MPV
    if (this->isAudio) {
        this->playOriginal();
        return;
    }
    // Prefer transcoded streams (no special headers required).
    logDebug("player: requesting streams");
    this->provider->videoStreams(
        this->file.id,
        [this, alive = this->alive](const std::vector<VideoStream>& streams,
                                    const std::string& def,
                                    const std::string& error) {
            brls::sync([this, alive, streams, def, error] {
                if (!*alive) return;
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
                this->startUrl(streams[this->streamIndex].url);
            });
        });
#endif
}

void PlayerPage::playOriginal() {
    logDebug("player: requesting download info");
    this->provider->downloadInfo(
        this->file.id,
        [this, alive = this->alive](const std::string& url,
                                    const std::string& headers,
                                    const std::string& error) {
            brls::sync([this, alive, url, headers, error] {
                if (!*alive) return;
                logDebug("player: download info err=" + error +
                         " urlLen=" + std::to_string(url.size()));
                if (!error.empty() || url.empty()) {
                    this->loadingLabel->setText(
                        error.empty() ? "cloud/player/no_stream"_i18n : error);
                    return;
                }
                this->startUrl(url, headers);
            });
        });
}

void PlayerPage::startUrl(const std::string& url, const std::string& headers) {
#ifdef NXKIT_WITH_MPV
    logDebug("player: startUrl hdr=" + std::to_string(headers.size()) +
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
            this->loadingLabel->setVisibility(brls::Visibility::VISIBLE);
            break;
        case MpvEventEnum::LOADING_END:
        case MpvEventEnum::MPV_LOADED:
            this->loadingLabel->setVisibility(brls::Visibility::GONE);
            break;
        case MpvEventEnum::MPV_FILE_ERROR:
            this->loadingLabel->setVisibility(brls::Visibility::VISIBLE);
            this->loadingLabel->setText("cloud/player/error"_i18n);
            this->retryWithSoftwareDec();
            break;
        case MpvEventEnum::END_OF_FILE:
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

void PlayerPage::updateOsd() {
#ifdef NXKIT_WITH_MPV
    auto& mpv = nxkit::MPVCore::instance();
    std::string text = wiliwili::sec2Time((int64_t)mpv.playback_time) + " / " +
                       wiliwili::sec2Time(mpv.duration);
    if (mpv.isPaused()) text += "  |  " + "cloud/player/paused"_i18n;
    if (this->streamIndex >= 0 && this->streamIndex < (int)this->streams.size())
        text += "  |  " + this->streams[this->streamIndex].resolution;
    this->osdProgress->setText(text);
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
    this->osdProgress->setText(
        wiliwili::sec2Time((int64_t)target) + " / " +
        wiliwili::sec2Time(mpv.duration) + "  |  " +
        (seconds > 0 ? "cloud/player/forward"_i18n : "cloud/player/backward"_i18n));
#else
    (void)seconds;
#endif
}

void PlayerPage::showQualityMenu() {
#ifdef NXKIT_WITH_MPV
    if (this->streams.empty()) return;
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
            this->startUrl(this->streams[selected].url);
        },
        this->streamIndex);
    brls::Application::pushActivity(new brls::Activity(dropdown));
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
