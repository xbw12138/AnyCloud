//
// cloud: custom empty / login / error state for the file browser.
//

#pragma once

#include <borealis.hpp>

#include <functional>
#include <string>

namespace cloud {

class CloudEmptyArt;

/// A centered illustration with a title, a description and an optional action.
/// Replaces the wiliwili empty-state picture used by RecyclingGrid so the
/// browser keeps a single, consistent visual language.
class CloudEmptyView : public brls::Box {
public:
    enum class Mode { Empty, Login, Error };

    CloudEmptyView();

    /// Switches the illustration and the text.
    void setContent(Mode mode, const std::string& title,
                    const std::string& detail);
    /// Shows the action button. An empty label hides it.
    void setAction(const std::string& label, std::function<void()> callback);

    /// View that should receive focus when the empty state replaces the list.
    brls::View* focusTarget();

private:
    CloudEmptyArt* art = nullptr;
    brls::Label* titleLabel = nullptr;
    brls::Label* detailLabel = nullptr;
    brls::Button* actionButton = nullptr;
    std::function<void()> actionCallback;
};

}  // namespace cloud
