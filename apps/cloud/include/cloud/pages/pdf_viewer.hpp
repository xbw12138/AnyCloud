//
// cloud: PDF page viewer backed by MuPDF.
//

#pragma once

#include <borealis.hpp>

#include <pthread.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

namespace cloud {

class PdfViewerPage : public brls::Box {
public:
    explicit PdfViewerPage(std::string path);
    ~PdfViewerPage() override;

    void onLayout() override;

private:
    class Canvas;

    struct RenderJob {
        int page = 0;
        float targetWidth = 0;
        float targetHeight = 0;
        unsigned generation = 0;
    };

    static void* workerEntry(void* arg);
    void workerLoop();
    void requestRender();
    void goToPage(int page);
    void zoomBy(float delta);
    void panBy(float dx, float dy);
    void updateStatus();

    std::string path;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    Canvas* canvas = nullptr;
    brls::Label* titleLabel = nullptr;
    brls::Label* statusLabel = nullptr;
    brls::Label* hintLabel = nullptr;

    std::mutex mutex;
    std::condition_variable condition;
    RenderJob job;
    bool hasJob = false;
    bool stopRequested = false;
    pthread_t worker = 0;
    bool workerStarted = false;

    unsigned generation = 0;
    int pageCount = 0;
    int pageIndex = 0;
    int inFlightPage = -1;
    float zoom = 1.0f;
    bool documentReady = false;
    bool failed = false;
    std::string failureText;

    // Last laid out canvas size; used to re-render after a resize.
    float lastCanvasWidth = 0;
    float lastCanvasHeight = 0;
};

}  // namespace cloud
