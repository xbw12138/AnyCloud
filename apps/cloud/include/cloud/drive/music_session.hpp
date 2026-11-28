//
// cloud: background music state shared by the player and the app shell.
//

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cloud/drive/drive_provider.hpp"

#ifdef NXKIT_WITH_MPV
#include "nxkit/media/mpv_core.hpp"
#endif

namespace cloud {

/// Music keeps playing after the audio player page is closed. The page is gone
/// at that point, so this singleton takes over: it remembers what is playing,
/// advances to the next track when one ends, and lets the app shell show a
/// small "now playing" overlay and push the player again.
///
/// Everything runs on the UI thread (mpv events hop through brls::sync), so no
/// locking is needed.
class MusicSession {
public:
    static MusicSession& instance();

    /// The audio player page is on screen: it owns playback and end-of-file
    /// handling, the overlay hides.
    void attach();
    /// The page closed while music played: keep going, advancing on our own.
    void background(bool shuffleMode, int loopMode);

    /// Describes the track that is playing, newest values win.
    void setTrack(const std::string& title, const std::string& artist,
                  std::shared_ptr<DriveProvider> provider,
                  const std::vector<DriveFile>& playlist, size_t index);
    void setPlaying(bool value);

    /// Playback was replaced or stopped: the overlay disappears.
    void clear();

    /// Music is playing and the player page is not open.
    bool active() const;
    bool playing() const;
    std::string title() const;
    std::string artist() const;

    /// Pushes the audio player again, resuming at the current position.
    void reopen();

private:
    MusicSession() = default;

    void subscribe();
    void unsubscribe();
    void onEndOfFile();
    /// Resolves the url of playlist[index] and starts it in mpv.
    void playIndex(size_t index);

    bool attached = false;
    bool activeFlag = false;
    bool playingFlag = false;
    bool shuffleMode = false;
    int loopMode = 0;
    std::string trackTitle;
    std::string trackArtist;
    std::shared_ptr<DriveProvider> provider;
    std::vector<DriveFile> playlist;
    size_t index = 0;

#ifdef NXKIT_WITH_MPV
    bool subscribed = false;
    nxkit::MpvEvent::Subscription eventSub;
#endif
};

}  // namespace cloud
