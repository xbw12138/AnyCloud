//
// cloud: fullscreen image viewer (L/R to flip pages).
//

#include "cloud/pages/image_viewer.hpp"

#include <exception>

#include "cpr/async.h"
#include "nxkit/http_client.hpp"

using namespace brls::literals;

namespace cloud {

namespace {

/// Baidu refuses plain GETs on some PCS links with 403 "user is not
/// authorized" and only answers bounded range requests for files it has not
/// fully cached on the node, so the image is pulled in 4 MB chunks and
/// stitched together. Returns the bytes and reports failure through `error`.
std::string fetchBaiduImage(const std::string& url, const std::string& headers,
                            std::string& error) {
    constexpr size_t kChunk = 4 * 1024 * 1024;
    constexpr size_t kLimit = 256 * 1024 * 1024;
    std::string body;
    uint64_t total = 0;
    while (true) {
        nxkit::HttpClient client;
        client.setHeaderBlock(headers);
        client.setTimeout(30000);
        const size_t offset = body.size();
        client.setHeader("Range", "bytes=" + std::to_string(offset) + "-" +
                                      std::to_string(offset + kChunk - 1));
        nxkit::HttpResult result = client.get(url);
        if (!result.ok) {
            error = result.error.empty()
                        ? "HTTP " + std::to_string(result.status)
                        : result.error;
            return std::string();
        }
        if (total == 0) {
            auto it = result.headers.find("content-range");
            if (it != result.headers.end()) {
                size_t slash = it->second.find('/');
                if (slash != std::string::npos) {
                    try {
                        total = std::stoull(it->second.substr(slash + 1));
                    } catch (...) {
                    }
                }
            }
        }
        body += result.body;
        // 200 means the server ignored the range and sent everything.
        if (result.status != 206) break;
        if (total > 0 && body.size() >= total) break;
        if (result.body.size() < kChunk) break;
        if (body.size() > kLimit) {
            error = "image too large";
            return std::string();
        }
    }
    if (body.empty()) error = "empty body";
    return body;
}

}  // namespace

ImageViewer::ImageViewer(std::shared_ptr<DriveProvider> provider,
                         std::vector<DriveFile> images, size_t index)
    : provider(std::move(provider)),
      images(std::move(images)),
      index(index) {
    this->setAxis(brls::Axis::COLUMN);
    this->setGrow(1);
    // Own the focus so the list cell behind us stops drawing its highlight.
    this->setFocusable(true);
    this->setHideHighlight(true);

    this->image = new brls::Image();
    this->image->setGrow(1);
    this->image->setScalingType(brls::ImageScalingType::FIT);
    this->addView(this->image);

    this->statusLabel = new brls::Label();
    this->statusLabel->setFontSize(18);
    this->statusLabel->setMargins(12, 16, 12, 16);
    this->addView(this->statusLabel);

    this->registerAction(
        "cloud/image/prev"_i18n, brls::BUTTON_LB,
        [this](brls::View*) -> bool {
            if (this->images.empty()) return true;
            this->index =
                (this->index + this->images.size() - 1) % this->images.size();
            this->showCurrent();
            return true;
        });
    this->registerAction(
        "cloud/image/next"_i18n, brls::BUTTON_RB,
        [this](brls::View*) -> bool {
            if (this->images.empty()) return true;
            this->index = (this->index + 1) % this->images.size();
            this->showCurrent();
            return true;
        });

    this->registerAction(
        "", brls::BUTTON_B,
        [](brls::View*) -> bool {
            brls::Application::popActivity();
            return true;
        },
        true);

    this->showCurrent();
}

ImageViewer::~ImageViewer() { *this->alive = false; }

void ImageViewer::showCurrent() {
    if (this->index >= this->images.size()) return;
    const DriveFile file = this->images[this->index];
    const size_t generation = ++this->generation;

    this->image->clear();
    this->statusLabel->setText("cloud/image/loading"_i18n);

    this->provider->downloadInfo(
        file.id,
        [this, alive = this->alive, generation](
            const std::string& url, const std::string& headers,
            const std::string& error) {
            if (!error.empty() || url.empty()) {
                brls::sync([this, alive, generation, error] {
                    if (!*alive || generation != this->generation) return;
                    this->statusLabel->setText(
                        error.empty() ? "cloud/image/error"_i18n : error);
                });
                return;
            }
            // The download url needs the provider's cookie; fetch the bytes
            // ourselves (ImageHelper cannot attach custom headers). Run on
            // cpr's worker pool: bare detached threads crash on the Switch.
            cpr::async([this, alive, generation, url, headers] {
                nxkit::HttpResult result;
                try {
                    if (this->provider->id() == "baidu") {
                        result.body = fetchBaiduImage(url, headers,
                                                      result.error);
                        result.ok = result.error.empty();
                    } else {
                        nxkit::HttpClient client;
                        client.setHeaderBlock(headers);
                        client.setTimeout(30000);
                        result = client.get(url);
                    }
                } catch (const std::exception& e) {
                    result.ok = false;
                    result.error = e.what();
                }
                brls::sync([this, alive, generation, result] {
                    if (!*alive || generation != this->generation) return;
                    if (!result.ok || result.body.empty()) {
                        this->statusLabel->setText(
                            result.error.empty() ? "cloud/image/error"_i18n
                                                 : result.error);
                        return;
                    }
                    this->image->setImageFromMem(
                        (unsigned char*)result.body.data(),
                        (int)result.body.size());
                    this->statusLabel->setText(
                        std::to_string(this->index + 1) + " / " +
                        std::to_string(this->images.size()));
                });
            });
        });
}

}  // namespace cloud
