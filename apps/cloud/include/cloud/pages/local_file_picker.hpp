//
// cloud: local file picker used by the upload action.
//

#pragma once

#include <borealis.hpp>

#include <functional>
#include <string>

namespace cloud {

class LocalFilePickerPage : public brls::Activity {
public:
    using PickCallback =
        std::function<void(const std::string& path, const std::string& name)>;

    explicit LocalFilePickerPage(PickCallback onPick,
                                 std::string startDirectory = std::string())
        : onPick(std::move(onPick)),
          startDirectory(std::move(startDirectory)) {}

    brls::View* createContentView() override;

private:
    void load(const std::string& path);

    PickCallback onPick;
    std::string startDirectory;
    std::string currentDirectory;
    brls::Box* listBox = nullptr;
    brls::Label* pathLabel = nullptr;
};

}  // namespace cloud
