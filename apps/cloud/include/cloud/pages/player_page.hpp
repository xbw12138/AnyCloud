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

/// Plays a video or audio file through the provider's streams.
/// Video mode renders fullscreen with a small OSD; audio mode shows a
/// centered title with progress.
class PlayerPage : public brls::Box {
public:
    PlayerPage(std::shared_ptr<DriveProvider> provider, DriveFile file,
               bool isAudio);
    ~PlayerPage() override;

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

private:
    void play();
    void playOriginal();
    void startUrl(const std::string& url, const std::string& headers = "");
    void onMpvEvent(int event);
    void updateOsd();
    void seekBy(int64_t seconds);
    void showQualityMenu();
    void presentQualityMenu();
    void retryWithSoftwareDec();
    bool savePosition();

    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    std::shared_ptr<DriveProvider> provider;
    DriveFile file;
    bool isAudio;

    std::vector<VideoStream> streams;
    int streamIndex = -1;

    std::string lastUrl;
    std::string lastHeaders;
    bool triedSoftwareDec = false;
    // Seconds to jump to once the stream is loaded (0 = play from start).
    double resumePosition = 0;

    brls::Label* loadingLabel = nullptr;
    brls::Label* titleLabel = nullptr;
    brls::Box* osdBox = nullptr;
    brls::Label* osdName = nullptr;
    brls::Label* osdProgress = nullptr;
    brls::Label* hintLabel = nullptr;

    bool mpvAvailable = false;

#ifdef NXKIT_WITH_MPV
    nxkit::MpvEvent::Subscription eventSub;
#endif
};

}  // namespace cloud
