//
// cloud: video / music playback page (libmpv).
//

#pragma once

#include <borealis.hpp>

#include <memory>
#include <string>
#include <vector>

#include "cloud/drive/drive_provider.hpp"

#ifdef NXKIT_WITH_MPV
#include "nxkit/media/mpv_core.hpp"
#endif

namespace cloud {

/// Thin seek bar drawn on the video OSD: dim track, accent fill and a knob.
class SeekBarView : public brls::View {
public:
    void setProgress(float value) {
        this->progress = value < 0 ? 0 : (value > 1 ? 1 : value);
        this->invalidate();
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

private:
    float progress = 0;
};

/// Plays a video or audio file through the provider's streams.
/// Video mode renders fullscreen with a bottom OSD (title, seek bar, hints)
/// that fades out while playing; audio mode shows a centered title with
/// progress. When the playlist holds more than one file, finishing one starts
/// the next (same directory playback).
class PlayerPage : public brls::Box {
public:
    PlayerPage(std::shared_ptr<DriveProvider> provider,
               std::vector<DriveFile> playlist, size_t index, bool isAudio);
    ~PlayerPage() override;

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

private:
    enum class LoopMode { Off, One, All };

    void play();
    void playOriginal();
    /// Starts the given playlist entry from the beginning.
    bool playAt(size_t index);
    /// Switches to the next playlist entry and starts it. Returns false when
    /// the current file is the last one.
    bool playNext();
    /// L / R: moves through the playlist, wrapping only in "loop all" mode.
    void playNextManual();
    void playPreviousManual();
    /// Cycles off -> one -> all and reports the new mode on screen.
    void toggleLoopMode();
    std::string loopLabel() const;
    /// PLUS: opens the directory list so any entry can be started directly.
    void showPlaylistMenu();
    void startUrl(const std::string& url, const std::string& headers = "");
    void onMpvEvent(int event);
    void updateOsd();
    /// Shows a short status line ("正在加载…", an error) instead of the
    /// resolution/paused meta text. An empty string restores the meta text.
    void setStatus(const std::string& text);
    /// Keeps the meta line under the player's control (loading / error).
    bool statusOverride = false;
    /// Reveals the video OSD and schedules it to fade out after a few seconds.
    /// `autoHide=false` keeps it on screen (paused / loading).
    void showOsd(bool autoHide = true);
    void seekBy(int64_t seconds);
    void showQualityMenu();
    void presentQualityMenu();
    void retryWithSoftwareDec();
    bool savePosition();

    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    std::shared_ptr<DriveProvider> provider;
    DriveFile file;
    bool isAudio;
    /// Files of the directory the current file was opened from, in list order.
    std::vector<DriveFile> playlist;
    size_t playlistIndex = 0;
    /// What happens when the last file ends (X cycles it).
    LoopMode loopMode = LoopMode::Off;

    std::vector<VideoStream> streams;
    int streamIndex = -1;

    std::string lastUrl;
    std::string lastHeaders;
    bool triedSoftwareDec = false;
    // Seconds to jump to once the stream is loaded (0 = play from start).
    double resumePosition = 0;

    brls::Label* loadingLabel = nullptr;
    brls::Label* titleLabel = nullptr;
    /// Video OSD: bottom gradient panel.
    brls::Box* osdPanel = nullptr;
    brls::Label* osdName = nullptr;
    brls::Label* metaLabel = nullptr;
    brls::Label* elapsedLabel = nullptr;
    brls::Label* durationLabel = nullptr;
    SeekBarView* seekBar = nullptr;
    /// Audio progress line.
    brls::Label* osdProgress = nullptr;
    brls::Label* hintLabel = nullptr;
    /// Pending `brls::delay` id used to hide the OSD.
    size_t osdTimer = 0;

    bool mpvAvailable = false;

#ifdef NXKIT_WITH_MPV
    nxkit::MpvEvent::Subscription eventSub;
#endif
};

}  // namespace cloud
