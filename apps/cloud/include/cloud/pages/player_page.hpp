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

    /// Track / knob colours, so the bar also reads on a light background.
    void setTrackColor(NVGcolor value) { this->trackColor = value; }
    void setThumbColor(NVGcolor value) { this->thumbColor = value; }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

private:
    float progress = 0;
    NVGcolor trackColor = nvgRGBA(255, 255, 255, 70);
    NVGcolor thumbColor = nvgRGBA(255, 255, 255, 255);
};

class AudioStageView;

/// Plays a video or audio file through the provider's streams.
/// Video mode renders fullscreen with a bottom OSD (title, seek bar, hints)
/// that fades out while playing; audio mode shows a widescreen dashboard with
/// a spinning record on the left and the queue on the right. When the playlist
/// holds more than one file, finishing one starts the next (same directory
/// playback).
class PlayerPage : public brls::Box {
public:
    PlayerPage(std::shared_ptr<DriveProvider> provider,
               std::vector<DriveFile> playlist, size_t index, bool isAudio,
               bool resumeInPlace = false);
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
    /// Audio: ↑ / ↓ move the D-pad cursor through the queue, A starts it.
    void moveSelection(int delta);
    /// Y on the audio page: shuffle the queue.
    void toggleShuffle();
    /// Next shuffled entry: each track is played once per round, and a new
    /// round starts only when `wrap` is set (loop all / manual skip). Returns
    /// playlist.size() when the round is over.
    size_t nextShuffled(bool wrap);
    /// PLUS: opens the directory list so any entry can be started directly.
    void showPlaylistMenu();
    void startUrl(const std::string& url, const std::string& headers = "");
    /// Some drive links only answer bounded (<= 4 MB) range requests, which
    /// the demuxer cannot use: pull the file to a local cache in chunks and
    /// play that copy instead.
    void cacheAndPlay();
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
    /// Replaces the control hint with `text` for a moment. `Application::notify`
    /// is a no-op in this borealis build, so player feedback needs the OSD.
    void showPlayerMessage(const std::string& text);
    int messageToken = 0;
    void seekBy(int64_t seconds);
    /// ↑ / ↓: volume, in 5% steps.
    void adjustVolume(int delta);
    /// Video: left-center volume meter, shown for a moment after ↑ / ↓.
    void showVolume(int percent);
    void drawVolume(NVGcontext* vg, float x, float y, float width,
                    float height);
    void showQualityMenu();
    void presentQualityMenu();
    /// L3: playback speed menu (0.5x - 2.0x).
    void showSpeedMenu();
    void applySpeed(double speed);
    /// R3: video aspect ratio menu (default, 16:9, 4:3, 1:1, 21:9, stretch).
    void showAspectMenu();
    void applyAspect(int index, bool notify);
    /// Looks for subtitle files next to the current video and auto-loads the
    /// one whose name matches the video.
    void requestSubtitles();
    /// Loads one page of the video's directory while collecting subtitles.
    void fetchSubtitlePage();
    /// MINUS: lists the subtitles the container embeds plus the subtitle files
    /// found next to the video, followed by the embedded audio tracks.
    void showTrackMenu();
    /// Downloads and activates subtitles[index], -1 turns subtitles off.
    void selectSubtitle(int index);

    /// One audio or subtitle track mpv found inside the played stream. Quark
    /// transcodes keep every audio track and the WebVTT subtitles of the
    /// original file in the HLS playlist, so switching them needs no provider
    /// request at all.
    struct MediaTrack {
        int64_t id = 0;
        std::string lang;
        std::string title;
        std::string codec;
        bool selected = false;
    };
    /// Title / language / codec of a track, for the menu and status lines.
    static std::string trackText(const MediaTrack& track);
    /// Re-reads mpv's track-list into audioTracks / embeddedSubtitles.
    void scanTracks();
    /// Activates one of the embedded tracks by its mpv id (-1 disables).
    void selectEmbeddedSubtitle(int64_t trackId);
    void selectAudioTrack(int64_t trackId);
    void retryWithSoftwareDec();
    bool savePosition();

    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    std::shared_ptr<DriveProvider> provider;
    DriveFile file;
    bool isAudio;
    /// Files of the directory the current file was opened from, in list order.
    std::vector<DriveFile> playlist;
    size_t playlistIndex = 0;
    /// Audio page: row the D-pad points at, and whether shuffle is on.
    size_t selectedIndex = 0;
    bool shuffle = false;
    /// Entries of the current shuffle round that have not played yet.
    std::vector<size_t> shuffleBag;
    /// True once a shuffle round has started, so an empty bag can mean
    /// "round finished" instead of "round not started".
    bool shuffleRoundUsed = false;
    /// What happens when the last file ends (X cycles it).
    LoopMode loopMode = LoopMode::Off;
    /// Playback speed picked with the L3 menu; reapplied on every load.
    double playbackSpeed = 1.0;
    /// Aspect ratio picked with the R3 menu; 0 keeps the video's own ratio.
    int aspectIndex = 0;

    std::vector<VideoStream> streams;
    int streamIndex = -1;

    /// Subtitle files found in the directory of the current video.
    std::vector<DriveFile> subtitles;
    /// Entry of `subtitles` that is active, -1 when subtitles are off or an
    /// embedded track is playing.
    int subtitleIndex = -1;
    /// Audio and subtitle tracks carried by the stream itself (mpv track-list).
    std::vector<MediaTrack> audioTracks;
    std::vector<MediaTrack> embeddedSubtitles;
    /// Id of the video `subtitles` were collected for, guards late replies.
    std::string subtitleScanId;
    std::string subtitleScanStem;
    int subtitleScanPage = 0;
    /// Local copy of the active subtitle, re-applied after a reload.
    std::string activeSubtitlePath;
    /// Set when a subtitle should be applied as soon as mpv finished loading.
    std::string pendingSubtitlePath;

    std::string lastUrl;
    std::string lastHeaders;
    /// Alternate headers (e.g. Baidu's netdisk UA + cookie) tried once when
    /// the initial browser identity is rejected by the CDN.
    std::string fallbackHeaders;
    bool triedFallbackHeaders = false;
    bool triedSoftwareDec = false;
    /// Set once the current url produced a loaded file. Software decoding is
    /// only worth retrying in that case: for a rejected network url the same
    /// link fails the same way and would just reload/teardown mpv again.
    bool loadedOnce = false;
    /// One-shot HTTP probe for a url mpv could not open: mpv only logs
    /// "failed to open", the status code is what tells a 403 from a timeout.
    bool probedUrl = false;
    /// Baidu CDN links are retried over plain http once: ffmpeg's mbedtls
    /// read fails on that host while the same url answers over http.
    bool triedHttpFallback = false;
    /// The url failed everywhere: for audio, download it into the local cache
    /// once and play that copy.
    bool triedLocalCache = false;
    // Seconds to jump to once the stream is loaded (0 = play from start).
    double resumePosition = 0;
    /// Audio reopened from the background: the stream is still running and
    /// must be taken over instead of reloaded.
    bool resumeInPlace = false;

    brls::Label* titleLabel = nullptr;
    /// Audio page: the whole-screen stage that draws the record and the queue.
    AudioStageView* stage = nullptr;
    /// Video OSD: bottom gradient panel.
    brls::Box* osdPanel = nullptr;
    brls::Label* osdName = nullptr;
    brls::Label* metaLabel = nullptr;
    brls::Label* elapsedLabel = nullptr;
    brls::Label* durationLabel = nullptr;
    SeekBarView* seekBar = nullptr;
    brls::Label* hintLabel = nullptr;
    /// Pending `brls::delay` id used to hide the OSD.
    size_t osdTimer = 0;
    /// Video volume meter: percentage to draw and how long it stays visible.
    int volumePercent = -1;
    bool volumeVisible = false;
    size_t volumeTimer = 0;

    bool mpvAvailable = false;

#ifdef NXKIT_WITH_MPV
    nxkit::MpvEvent::Subscription eventSub;
#endif
};

}  // namespace cloud
