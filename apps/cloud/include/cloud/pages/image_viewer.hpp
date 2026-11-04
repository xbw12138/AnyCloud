//
// cloud: fullscreen image viewer (L/R to flip pages).
//

#pragma once

#include <borealis.hpp>

#include <memory>
#include <string>
#include <vector>

#include "cloud/drive/drive_provider.hpp"

namespace cloud {

class ImageViewer : public brls::Box {
public:
    ImageViewer(std::shared_ptr<DriveProvider> provider,
                std::vector<DriveFile> images, size_t index);
    ~ImageViewer() override;

private:
    void showCurrent();

    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    std::shared_ptr<DriveProvider> provider;
    std::vector<DriveFile> images;
    size_t index = 0;
    /// Bumped on every navigation to drop stale download results.
    size_t generation = 0;

    brls::Image* image = nullptr;
    brls::Label* statusLabel = nullptr;
};

}  // namespace cloud
