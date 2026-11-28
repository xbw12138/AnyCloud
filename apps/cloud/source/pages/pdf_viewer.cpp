//
// cloud: PDF page viewer backed by MuPDF.
//

#include "cloud/pages/pdf_viewer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "cloud/ui/now_playing_overlay.hpp"
#include "nxkit/paths.hpp"

#include <mupdf/fitz.h>
#include <nlohmann/json.hpp>

using namespace brls::literals;

namespace cloud {

namespace {

/// Where the last opened page of each document is remembered.
const char* kPageStoreFile = "pdf_pages.json";

std::string fileNameOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/** Page (1-based) the user stopped on last time, 0 when unknown. */
int loadLastPage(const std::string& path) {
    if (path.empty()) return 0;
    std::ifstream input(nxkit::dataPath(kPageStoreFile));
    if (!input.is_open()) return 0;
    nlohmann::json root = nlohmann::json::parse(input, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return 0;
    auto it = root.find(path);
    if (it == root.end() || !it->is_number_integer()) return 0;
    return it->get<int>();
}

/** Remembers `page` (1-based) for `path`. */
void saveLastPage(const std::string& path, int page) {
    if (path.empty() || page <= 0) return;

    nlohmann::json root = nlohmann::json::object();
    {
        // Scoped: the SD card filesystem refuses the write below while this
        // handle is still open on the same path.
        std::ifstream input(nxkit::dataPath(kPageStoreFile));
        if (input.is_open()) {
            nlohmann::json stored =
                nlohmann::json::parse(input, nullptr, false);
            if (!stored.is_discarded() && stored.is_object())
                root = std::move(stored);
        }
    }

    auto it = root.find(path);
    if (it != root.end() && it->is_number_integer() &&
        it->get<int>() == page)
        return;

    root[path] = page;
    nxkit::ensureDataDirectory();
    std::ofstream output(nxkit::dataPath(kPageStoreFile));
    if (!output.is_open()) return;
    output << root.dump();
}


/** Renders a single page into an RGBA buffer with a white background. */
bool renderPage(fz_context* ctx, fz_document* document, int pageNumber,
                float targetWidth, float targetHeight,
                std::vector<unsigned char>* rgba, int* outWidth,
                int* outHeight, std::string* error) {
    fz_page* page = nullptr;
    fz_pixmap* pixmap = nullptr;
    fz_var(page);
    fz_var(pixmap);

    bool ok = false;
    fz_try(ctx) {
        page = fz_load_page(ctx, document, pageNumber);
        fz_rect bounds = fz_bound_page(ctx, page);
        float pageWidth = bounds.x1 - bounds.x0;
        float pageHeight = bounds.y1 - bounds.y0;
        if (pageWidth <= 0 || pageHeight <= 0)
            fz_throw(ctx, FZ_ERROR_GENERIC, "empty page");

        float scale = std::min(targetWidth / pageWidth,
                               targetHeight / pageHeight);
        if (!(scale > 0)) scale = 1.0f;
        fz_matrix ctm = fz_scale(scale, scale);
        pixmap =
            fz_new_pixmap_from_page(ctx, page, ctm, fz_device_rgb(ctx), 0);

        int width = fz_pixmap_width(ctx, pixmap);
        int height = fz_pixmap_height(ctx, pixmap);
        int components = fz_pixmap_components(ctx, pixmap);
        int stride = fz_pixmap_stride(ctx, pixmap);
        unsigned char* samples = fz_pixmap_samples(ctx, pixmap);
        if (!samples || width <= 0 || height <= 0 || components < 1)
            fz_throw(ctx, FZ_ERROR_GENERIC, "invalid pixmap");

        rgba->resize((size_t)width * (size_t)height * 4);
        for (int y = 0; y < height; y++) {
            const unsigned char* src = samples + (size_t)y * stride;
            unsigned char* dst = rgba->data() + (size_t)y * width * 4;
            if (components >= 3) {
                for (int x = 0; x < width; x++) {
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = components >= 4 ? src[3] : 255;
                    src += components;
                    dst += 4;
                }
            } else {
                for (int x = 0; x < width; x++) {
                    dst[0] = src[0];
                    dst[1] = src[0];
                    dst[2] = src[0];
                    dst[3] = components >= 2 ? src[1] : 255;
                    src += components;
                    dst += 4;
                }
            }
        }
        *outWidth = width;
        *outHeight = height;
        ok = true;
    }
    fz_always(ctx) {
        fz_drop_pixmap(ctx, pixmap);
        fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        if (error) *error = fz_caught_message(ctx);
    }
    return ok;
}

}  // namespace

class PdfViewerPage::Canvas : public brls::Box {
public:
    explicit Canvas(NVGcolor background) : background(background) {
        this->setWidthPercentage(100);
        this->setGrow(1);
        this->setClipsToBounds(true);
    }

    ~Canvas() override { this->clear(); }

    void setPageData(const std::vector<unsigned char>& rgba, int width,
                     int height) {
        NVGcontext* vg = brls::Application::getNVGContext();
        if (!vg) return;
        int texture =
            nvgCreateImageRGBA(vg, width, height, 0, rgba.data());
        if (texture == 0) return;
        this->clear();
        this->texture = texture;
        this->imageWidth = width;
        this->imageHeight = height;
        this->panX = 0;
        this->panY = 0;
        this->invalidate();
    }

    void clear() {
        NVGcontext* vg = brls::Application::getNVGContext();
        if (this->texture != 0 && vg) nvgDeleteImage(vg, this->texture);
        this->texture = 0;
        this->imageWidth = 0;
        this->imageHeight = 0;
    }

    bool hasPage() const { return this->texture != 0; }

    void setZoom(float value) {
        this->zoom = value;
        this->panX = 0;
        this->panY = 0;
        this->invalidate();
    }

    void panBy(float dx, float dy) {
        if (this->zoom <= 1.001f) return;
        float maxX = std::max(0.0f, (this->drawnWidth - this->viewWidth) * 0.5f);
        float maxY =
            std::max(0.0f, (this->drawnHeight - this->viewHeight) * 0.5f);
        this->panX = std::max(-maxX, std::min(maxX, this->panX + dx));
        this->panY = std::max(-maxY, std::min(maxY, this->panY + dy));
        this->invalidate();
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillColor(vg, this->background);
        nvgFill(vg);

        if (this->texture == 0 || this->imageWidth <= 0 ||
            this->imageHeight <= 0 || width <= 0 || height <= 0)
            return;

        float fit = std::min(width / (float)this->imageWidth,
                             height / (float)this->imageHeight);
        float scale = fit * this->zoom;
        float drawnWidth = this->imageWidth * scale;
        float drawnHeight = this->imageHeight * scale;
        this->viewWidth = width;
        this->viewHeight = height;
        this->drawnWidth = drawnWidth;
        this->drawnHeight = drawnHeight;

        float offsetX = (width - drawnWidth) * 0.5f;
        float offsetY = (height - drawnHeight) * 0.5f;
        if (drawnWidth > width)
            offsetX = std::max(width - drawnWidth,
                               std::min(0.0f, offsetX + this->panX));
        if (drawnHeight > height)
            offsetY = std::max(height - drawnHeight,
                               std::min(0.0f, offsetY + this->panY));

        nvgSave(vg);
        nvgIntersectScissor(vg, x, y, width, height);
        NVGpaint paint =
            nvgImagePattern(vg, x + offsetX, y + offsetY, drawnWidth,
                            drawnHeight, 0, this->texture, 1.0f);
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillPaint(vg, paint);
        nvgFill(vg);
        nvgRestore(vg);
    }

private:
    NVGcolor background;
    int texture = 0;
    int imageWidth = 0;
    int imageHeight = 0;
    float zoom = 1.0f;
    float panX = 0;
    float panY = 0;
    float viewWidth = 1;
    float viewHeight = 1;
    float drawnWidth = 1;
    float drawnHeight = 1;
};

PdfViewerPage::PdfViewerPage(std::string path) : path(std::move(path)) {
    this->setAxis(brls::Axis::COLUMN);
    this->setGrow(1);
    this->setFocusable(true);
    this->setHideHighlight(true);
    this->setBackgroundColor(brls::Application::getTheme()["cloud/bg"]);
    this->setPadding(20, 48, 16, 48);

    brls::Theme theme = brls::Application::getTheme();

    // Reopen on the page the user stopped at last time (stored 1-based).
    this->pageIndex = std::max(0, loadLastPage(this->path) - 1);

    this->titleLabel = new brls::Label();
    this->titleLabel->setFontSize(16);
    this->titleLabel->setHeight(22);
    this->titleLabel->setTextColor(theme["cloud/text_secondary"]);
    this->titleLabel->setText(fileNameOf(this->path));
    this->addView(this->titleLabel);

    this->statusLabel = new brls::Label();
    this->statusLabel->setFontSize(12);
    this->statusLabel->setHeight(18);
    this->statusLabel->setMarginTop(2);
    this->statusLabel->setTextColor(theme["cloud/text_secondary"]);
    this->addView(this->statusLabel);

    this->canvas = new Canvas(theme["cloud/surface_alt"]);
    this->canvas->setMarginTop(10);
    this->addView(this->canvas);

    this->hintLabel = new brls::Label();
    this->hintLabel->setFontSize(12);
    this->hintLabel->setHeight(18);
    this->hintLabel->setMarginTop(8);
    this->hintLabel->setTextColor(theme["cloud/text_secondary"]);
    this->hintLabel->setText("cloud/pdf/hint"_i18n);
    this->addView(this->hintLabel);

    // Background music keeps playing while reading: show the "now playing"
    // pill here too (R3 returns to the player).
    attachNowPlayingOverlay(this);

    this->registerAction(
        "cloud/pdf/prev"_i18n, brls::BUTTON_LB,
        [this](brls::View*) -> bool {
            this->goToPage(this->pageIndex - 1);
            return true;
        });
    this->registerAction(
        "cloud/pdf/next"_i18n, brls::BUTTON_RB,
        [this](brls::View*) -> bool {
            this->goToPage(this->pageIndex + 1);
            return true;
        });
    this->registerAction(
        "", brls::BUTTON_NAV_LEFT,
        [this](brls::View*) -> bool {
            if (this->zoom > 1.001f)
                this->panBy(48, 0);
            else
                this->goToPage(this->pageIndex - 1);
            return true;
        },
        true, true);
    this->registerAction(
        "", brls::BUTTON_NAV_RIGHT,
        [this](brls::View*) -> bool {
            if (this->zoom > 1.001f)
                this->panBy(-48, 0);
            else
                this->goToPage(this->pageIndex + 1);
            return true;
        },
        true, true);
    this->registerAction(
        "", brls::BUTTON_NAV_UP,
        [this](brls::View*) -> bool {
            this->panBy(0, 48);
            return true;
        },
        true, true);
    this->registerAction(
        "", brls::BUTTON_NAV_DOWN,
        [this](brls::View*) -> bool {
            this->panBy(0, -48);
            return true;
        },
        true, true);
    this->registerAction(
        "cloud/pdf/zoom_in"_i18n, brls::BUTTON_Y,
        [this](brls::View*) -> bool {
            this->zoomBy(0.5f);
            return true;
        });
    this->registerAction(
        "cloud/pdf/zoom_out"_i18n, brls::BUTTON_X,
        [this](brls::View*) -> bool {
            this->zoomBy(-0.5f);
            return true;
        });
    this->registerAction(
        "", brls::BUTTON_B,
        [](brls::View*) -> bool {
            brls::Application::popActivity();
            return true;
        },
        true);

    this->updateStatus();

    // Give MuPDF a generous stack: rendering deeply nested content streams
    // needs more than the default homebrew thread stack.
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 2 * 1024 * 1024);
    this->workerStarted =
        pthread_create(&this->worker, &attributes, &PdfViewerPage::workerEntry,
                       this) == 0;
    pthread_attr_destroy(&attributes);

    this->requestRender();
}

PdfViewerPage::~PdfViewerPage() {
    *this->alive = false;
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        this->stopRequested = true;
    }
    this->condition.notify_all();
    if (this->workerStarted) pthread_join(this->worker, nullptr);
}

void* PdfViewerPage::workerEntry(void* arg) {
    static_cast<PdfViewerPage*>(arg)->workerLoop();
    return nullptr;
}

void PdfViewerPage::onLayout() {
    brls::Box::onLayout();
    if (!this->canvas) return;
    float width = this->canvas->getWidth();
    float height = this->canvas->getHeight();
    if (width <= 0 || height <= 0) return;
    if (std::fabs(width - this->lastCanvasWidth) < 1.0f &&
        std::fabs(height - this->lastCanvasHeight) < 1.0f)
        return;
    this->lastCanvasWidth = width;
    this->lastCanvasHeight = height;
    this->requestRender();
}

void PdfViewerPage::requestRender() {
    if (!this->canvas || this->failed) return;
    float width = this->canvas->getWidth();
    float height = this->canvas->getHeight();
    if (width <= 0) width = this->getWidth();
    if (width <= 0) width = 1184;
    if (height <= 0) height = this->getHeight() - 110;
    if (height <= 0) height = 540;

    // Supersample so docked output and zoomed pages stay sharp.
    float targetWidth = width * this->zoom * 1.5f;
    float targetHeight = height * this->zoom * 1.5f;
    float longest = std::max(targetWidth, targetHeight);
    if (longest > 2600.0f) {
        float factor = 2600.0f / longest;
        targetWidth *= factor;
        targetHeight *= factor;
    }

    RenderJob next;
    next.page = this->pageIndex;
    next.targetWidth = targetWidth;
    next.targetHeight = targetHeight;
    next.generation = ++this->generation;
    this->inFlightPage = this->pageIndex;
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        this->job = next;
        this->hasJob = true;
    }
    this->condition.notify_one();
    this->updateStatus();
}

void PdfViewerPage::goToPage(int page) {
    if (this->pageCount <= 0) return;
    if (page < 0) page = 0;
    if (page > this->pageCount - 1) page = this->pageCount - 1;
    if (page == this->pageIndex && this->canvas->hasPage() &&
        this->inFlightPage < 0)
        return;
    this->pageIndex = page;
    saveLastPage(this->path, page + 1);
    this->canvas->setZoom(this->zoom);
    this->requestRender();
}

void PdfViewerPage::zoomBy(float delta) {
    float value = this->zoom + delta;
    if (value < 1.0f) value = 1.0f;
    if (value > 3.0f) value = 3.0f;
    if (std::fabs(value - this->zoom) < 0.01f) return;
    this->zoom = value;
    this->canvas->setZoom(this->zoom);
    this->requestRender();
}

void PdfViewerPage::panBy(float dx, float dy) {
    if (this->zoom <= 1.001f) return;
    this->canvas->panBy(dx, dy);
}

void PdfViewerPage::updateStatus() {
    if (this->failed) {
        this->statusLabel->setText("cloud/pdf/error"_i18n);
        return;
    }
    if (!this->documentReady) {
        this->statusLabel->setText("cloud/pdf/loading"_i18n);
        return;
    }
    std::string text =
        brls::getStr("cloud/pdf/page", this->pageIndex + 1, this->pageCount);
    if (this->inFlightPage == this->pageIndex)
        text += "   " + "cloud/pdf/loading"_i18n;
    if (this->zoom > 1.001f) {
        text += "   " + std::to_string((int)std::lround(this->zoom * 100)) +
                "%";
    }
    this->statusLabel->setText(text);
}

void PdfViewerPage::workerLoop() {
    fz_context* ctx = nullptr;
    fz_document* document = nullptr;

    while (true) {
        RenderJob current;
        {
            std::unique_lock<std::mutex> lock(this->mutex);
            this->condition.wait(
                lock, [this] { return this->stopRequested || this->hasJob; });
            if (this->stopRequested) break;
            current = this->job;
            this->hasJob = false;
        }

        if (!ctx) {
            ctx = fz_new_context(nullptr, nullptr, 64u << 20);
            if (!ctx) {
                brls::sync([this, alive = this->alive] {
                    if (!*alive) return;
                    this->failed = true;
                    this->updateStatus();
                });
                continue;
            }
            std::string error;
            int count = 0;
            fz_var(document);
            fz_var(count);
            fz_try(ctx) {
                fz_register_document_handlers(ctx);
                document = fz_open_document(ctx, this->path.c_str());
                count = fz_count_pages(ctx, document);
            }
            fz_catch(ctx) {
                error = fz_caught_message(ctx);
            }
            if (count <= 0) {
                if (document) {
                    fz_drop_document(ctx, document);
                    document = nullptr;
                }
                brls::Logger::error("pdf: cannot open {}: {}", this->path,
                                    error);
                brls::sync([this, alive = this->alive] {
                    if (!*alive) return;
                    this->failed = true;
                    this->updateStatus();
                });
                continue;
            }
            brls::sync([this, alive = this->alive, count] {
                if (!*alive) return;
                this->pageCount = count;
                this->documentReady = true;
                if (this->pageIndex >= this->pageCount)
                    this->pageIndex = this->pageCount - 1;
                this->updateStatus();
                this->requestRender();
            });
        }

        std::vector<unsigned char> rgba;
        int width = 0;
        int height = 0;
        std::string error;
        bool ok = renderPage(ctx, document, current.page, current.targetWidth,
                             current.targetHeight, &rgba, &width, &height,
                             &error);
        int page = current.page;
        unsigned jobGeneration = current.generation;
        brls::sync(
            [this, alive = this->alive, jobGeneration, page, ok,
             rgba = std::move(rgba), width, height] {
                if (!*alive) return;
                if (jobGeneration != this->generation) return;
                this->inFlightPage = -1;
                if (!ok || rgba.empty() || width <= 0 || height <= 0) {
                    this->failed = true;
                    this->updateStatus();
                    return;
                }
                this->pageIndex = page;
                this->canvas->setPageData(rgba, width, height);
                this->updateStatus();
            });
    }

    if (document) fz_drop_document(ctx, document);
    if (ctx) fz_drop_context(ctx);
}

}  // namespace cloud
