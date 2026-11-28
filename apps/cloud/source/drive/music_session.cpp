//
// cloud: background music state shared by the player and the app shell.
//

#include "cloud/drive/music_session.hpp"

#include <algorithm>
#include <chrono>
#include <random>

#include <borealis.hpp>

#include "cloud/drive/playback_store.hpp"
#include "cloud/pages/player_page.hpp"
#include "cloud/util/audio_name.hpp"
#include "cloud/util/http_headers.hpp"

namespace cloud {

namespace {

std::mt19937& shuffleRng() {
    static std::mt19937 rng(
        (unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
    return rng;
}

}  // namespace

MusicSession& MusicSession::instance() {
    static MusicSession session;
    return session;
}

void MusicSession::attach() {
    this->attached = true;
    this->unsubscribe();
}

void MusicSession::background(bool shuffle, int loop) {
    this->attached = false;
    this->shuffleMode = shuffle;
    this->loopMode = loop;
    this->activeFlag = true;
    this->subscribe();
}

void MusicSession::setTrack(const std::string& title,
                            const std::string& artist,
                            std::shared_ptr<DriveProvider> provider,
                            const std::vector<DriveFile>& playlist,
                            size_t index) {
    this->trackTitle = title;
    this->trackArtist = artist;
    this->provider = std::move(provider);
    this->playlist = playlist;
    this->index = index;
    this->activeFlag = true;
}

void MusicSession::setPlaying(bool value) {
    this->playingFlag = value;
    if (value) this->activeFlag = true;
}

void MusicSession::clear() {
    this->unsubscribe();
    this->activeFlag = false;
    this->playingFlag = false;
    this->trackTitle.clear();
    this->trackArtist.clear();
    this->provider.reset();
    this->playlist.clear();
    this->index = 0;
}

bool MusicSession::active() const { return this->activeFlag && !this->attached; }

bool MusicSession::playing() const { return this->playingFlag; }

std::string MusicSession::title() const { return this->trackTitle; }

std::string MusicSession::artist() const { return this->trackArtist; }

void MusicSession::subscribe() {
#ifdef NXKIT_WITH_MPV
    if (this->subscribed) return;
    this->subscribed = true;
    this->eventSub = nxkit::MPVCore::instance().getEvent()->subscribe(
        [this](nxkit::MpvEventEnum event) {
            if (event != nxkit::MpvEventEnum::END_OF_FILE) return;
            // Events arrive on mpv's thread; everything here is UI state.
            brls::sync([this] { this->onEndOfFile(); });
        });
#endif
}

void MusicSession::unsubscribe() {
#ifdef NXKIT_WITH_MPV
    if (!this->subscribed) return;
    this->subscribed = false;
    nxkit::MPVCore::instance().getEvent()->unsubscribe(this->eventSub);
#endif
}

void MusicSession::onEndOfFile() {
    // The player page was opened again in the meantime and handles this itself.
    if (this->attached) return;
    if (this->playlist.empty() || !this->provider) {
        this->clear();
        return;
    }

    // The finished track is done: no resume point next time.
    if (this->index < this->playlist.size()) {
        PlaybackStore::instance().update(this->playlist[this->index].id, 0, 0);
    }

    size_t next = this->playlist.size();
    if (this->loopMode == 1) {
        next = this->index;
    } else if (this->shuffleMode) {
        if (this->playlist.size() > 1) {
            std::uniform_int_distribution<size_t> dist(
                0, this->playlist.size() - 1);
            next = dist(shuffleRng());
            if (next == this->index)
                next = (next + 1) % this->playlist.size();
        } else {
            next = this->loopMode == 2 ? 0 : this->playlist.size();
        }
    } else if (this->index + 1 < this->playlist.size()) {
        next = this->index + 1;
    } else if (this->loopMode == 2) {
        next = 0;
    }

    if (next >= this->playlist.size()) {
        this->clear();
        return;
    }
    this->playIndex(next);
}

void MusicSession::playIndex(size_t at) {
    if (at >= this->playlist.size() || !this->provider) return;

    this->index = at;
    const DriveFile file = this->playlist[at];
    const AudioName info = describeAudio(file.name);
    this->trackTitle = info.title;
    this->trackArtist = info.artist;
    this->activeFlag = true;
    this->playingFlag = true;

    std::shared_ptr<DriveProvider> provider = this->provider;
    const std::string fileId = file.id;
    provider->downloadInfo(
        fileId,
        [this, provider, fileId](const std::string& url,
                                 const std::string& headers,
                                 const std::string& error) {
            // The player page took over while the lookup was running.
            if (this->attached) return;
            if (!error.empty() || url.empty()) {
                this->clear();
                return;
            }
            const std::string playback = provider->playbackHeaders();
            brls::sync([this, url,
                        fields = playback.empty() ? headers : playback] {
                if (this->attached) return;
                startMpvStream(url, fields);
            });
        });
}

void MusicSession::reopen() {
    if (this->attached || this->playlist.empty()) return;

    std::shared_ptr<DriveProvider> playerProvider = this->provider;
    std::vector<DriveFile> queue = this->playlist;
    const size_t at = this->index;

#ifdef NXKIT_WITH_MPV
    // The stream is still running: the page takes it over instead of resolving
    // and buffering the url again, which left a short silence.
    const bool live = !nxkit::MPVCore::instance().isStopped();
#else
    const bool live = false;
#endif

    brls::Application::pushActivity(new brls::Activity(
        new PlayerPage(playerProvider, std::move(queue), at, true, live)));
}

}  // namespace cloud
