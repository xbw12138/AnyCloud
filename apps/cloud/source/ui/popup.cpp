//
// cloud: custom card popups.
//

#include "cloud/ui/popup.hpp"

using namespace brls::literals;

namespace cloud {

namespace {

/// Readable text colour for a filled background, picked from its luminance so
/// the light and dark theme accents both stay legible.
NVGcolor onColor(NVGcolor background) {
    float luminance = 0.299f * background.r + 0.587f * background.g +
                      0.114f * background.b;
    return luminance > 0.50f ? nvgRGB(18, 20, 26) : nvgRGB(255, 255, 255);
}

const NVGcolor kDanger = nvgRGB(236, 96, 96);

/// Popup host that keeps the underlying page rendering, so the scrim reads as
/// a dimmed page rather than an opaque sheet.
class PopupActivity : public brls::Activity {
public:
    explicit PopupActivity(brls::View* content) : brls::Activity(content) {}

    bool isTranslucent() override { return true; }
};

}  // namespace

PillButton::PillButton(const std::string& text, bool danger,
                       std::function<void()> onClick)
    : danger(danger), onClick(std::move(onClick)) {
    this->setAxis(brls::Axis::ROW);
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setJustifyContent(brls::JustifyContent::CENTER);
    this->setHeight(42);
    this->setPadding(0, 20, 0, 20);
    this->setCornerRadius(21);
    // The default highlight paints a translucent wash over the fill, which
    // makes the pill look washed out; the fill itself signals focus.
    this->setHideHighlight(true);
    this->setMarginRight(10);
    this->setFocusable(true);

    this->label = new brls::Label();
    this->label->setText(text);
    this->label->setFontSize(15);
    this->label->setHeight(20);
    this->addView(this->label);

    this->registerAction("hints/ok"_i18n, brls::BUTTON_A,
                         [this](brls::View*) {
                             if (this->onClick) this->onClick();
                             return true;
                         });
    this->registerClickAction([this](brls::View*) {
        if (this->onClick) this->onClick();
        return true;
    });
    this->refreshStyle();
}

void PillButton::onFocusGained() {
    brls::Box::onFocusGained();
    this->refreshStyle();
}

void PillButton::onFocusLost() {
    brls::Box::onFocusLost();
    this->refreshStyle();
}

void PillButton::dropTrailingMargin() { this->setMarginRight(0); }

void PillButton::refreshStyle() {
    brls::Theme theme = brls::Application::getTheme();
    if (this->isFocused()) {
        NVGcolor fill = this->danger ? kDanger : theme["brls/accent"];
        this->setBackgroundColor(fill);
        this->setBorderThickness(0);
        this->label->setTextColor(onColor(fill));
    } else {
        this->setBackgroundColor(theme["cloud/surface_alt"]);
        this->setBorderColor(theme["cloud/border"]);
        this->setBorderThickness(1);
        this->label->setTextColor(this->danger ? kDanger
                                               : theme["cloud/text"]);
    }
}

brls::Box* makePopupCard(float widthPercent) {
    brls::Theme theme = brls::Application::getTheme();

    brls::Box* card = new brls::Box(brls::Axis::COLUMN);
    card->setWidthPercentage(widthPercent);
    card->setPadding(24, 26, 22, 26);
    card->setCornerRadius(20);
    card->setBackgroundColor(theme["cloud/surface"]);
    card->setBorderThickness(1);
    card->setBorderColor(theme["cloud/border"]);
    return card;
}

/// Dims the page and shows `card` in the middle. The popup lives in its own
/// activity so the page underneath keeps its state.
void presentPopup(brls::Box* card, brls::View* initialFocus) {
    brls::Box* root = new brls::Box(brls::Axis::COLUMN);
    root->setAlignItems(brls::AlignItems::CENTER);
    root->setJustifyContent(brls::JustifyContent::CENTER);
    root->setBackgroundColor(nvgRGBA(0, 0, 0, 120));
    root->addView(card);

    // Borealis does not pop activities on B by itself.
    root->registerAction("hints/back"_i18n, brls::BUTTON_B, [](brls::View*) {
        brls::Application::popActivity();
        return true;
    });

    brls::Application::pushActivity(new PopupActivity(root));
    // pushActivity() only focuses the content view, not a child.
    if (initialFocus) {
        brls::sync([initialFocus] {
            brls::Application::giveFocus(initialFocus);
        });
    }
}

void presentConfirm(const std::string& title, const std::string& message,
                    const std::string& confirmText, bool danger,
                    std::function<void()> onConfirm) {
    brls::Theme theme = brls::Application::getTheme();

    brls::Box* card = makePopupCard(46);

    brls::Label* titleLabel = new brls::Label();
    titleLabel->setText(title);
    titleLabel->setFontSize(19);
    titleLabel->setHeight(24);
    titleLabel->setTextColor(theme["cloud/text"]);
    card->addView(titleLabel);

    brls::Label* messageLabel = new brls::Label();
    messageLabel->setText(message);
    messageLabel->setFontSize(13);
    messageLabel->setMarginTop(10);
    messageLabel->setTextColor(theme["cloud/text_secondary"]);
    card->addView(messageLabel);

    brls::Box* actions = new brls::Box(brls::Axis::ROW);
    actions->setAlignItems(brls::AlignItems::CENTER);
    actions->setJustifyContent(brls::JustifyContent::FLEX_END);
    actions->setWidthPercentage(100);
    actions->setMarginTop(22);

    PillButton* cancel =
        new PillButton("cloud/files/action/cancel"_i18n, false, [] {
            brls::Application::popActivity();
        });
    actions->addView(cancel);

    PillButton* confirm =
        new PillButton(confirmText, danger, [onConfirm] {
            if (onConfirm) onConfirm();
            brls::Application::popActivity();
        });
    confirm->dropTrailingMargin();
    actions->addView(confirm);

    card->addView(actions);
    presentPopup(card, confirm);
}

}  // namespace cloud
