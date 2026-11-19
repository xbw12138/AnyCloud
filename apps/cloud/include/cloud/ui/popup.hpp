//
// cloud: custom card popups (scrim + rounded card + pill buttons).
//

#pragma once

#include <borealis.hpp>

#include <functional>
#include <string>

namespace cloud {

/// Rounded action button used by the popups. Focus is drawn by filling the
/// pill instead of the default highlight overlay.
class PillButton : public brls::Box {
public:
    PillButton(const std::string& text, bool danger,
               std::function<void()> onClick);

    void onFocusGained() override;
    void onFocusLost() override;

    void dropTrailingMargin();

private:
    void refreshStyle();

    bool danger;
    std::function<void()> onClick;
    brls::Label* label = nullptr;
};

/// Rounded card used as the popup surface.
brls::Box* makePopupCard(float widthPercent);

/// Dims the current page and centres `card`. The popup is pushed as a
/// translucent activity so the page stays visible underneath.
void presentPopup(brls::Box* card, brls::View* initialFocus);

/// Two-button confirmation card (cancel + confirm).
void presentConfirm(const std::string& title, const std::string& message,
                    const std::string& confirmText, bool danger,
                    std::function<void()> onConfirm);

}  // namespace cloud
