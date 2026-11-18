//
// cloud: local directory picker used by "download to..." actions.
//

#pragma once

#include <borealis.hpp>

#include <functional>
#include <string>

namespace cloud {

class LocalDirPage : public brls::Activity {
public:
    using PickCallback = std::function<void(const std::string& directory)>;

    explicit LocalDirPage(PickCallback onPick,
                          std::string startDirectory = std::string())
        : onPick(std::move(onPick)),
          startDirectory(std::move(startDirectory)) {}

    brls::View* createContentView() override;

private:
    void load(const std::string& path);
    void promptNewDir();
    void confirm();

    PickCallback onPick;
    std::string startDirectory;
    std::string currentDirectory;
    brls::Box* listBox = nullptr;
    brls::Label* pathLabel = nullptr;
};

}  // namespace cloud
