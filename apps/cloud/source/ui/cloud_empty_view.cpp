//
// cloud: custom empty / login / error state for the file browser.
//

#include "cloud/ui/cloud_empty_view.hpp"

#include <algorithm>

namespace cloud {

namespace {

constexpr float kPi = 3.14159265358979323846f;

NVGcolor fade(NVGcolor color, float alpha) {
    color.a *= alpha;
    return color;
}

void drawCloud(NVGcontext* vg, float cx, float cy, float s, NVGcolor fill) {
    nvgBeginPath(vg);
    nvgCircle(vg, cx - 18 * s, cy + 2 * s, 14 * s);
    nvgCircle(vg, cx + 4 * s, cy - 4 * s, 20 * s);
    nvgCircle(vg, cx + 23 * s, cy + 3 * s, 13 * s);
    nvgRoundedRect(vg, cx - 25 * s, cy + 1 * s, 52 * s, 16 * s, 8 * s);
    nvgFillColor(vg, fill);
    nvgFill(vg);
}

void drawQuarkMark(NVGcontext* vg, float cx, float cy, float s, NVGcolor fill,
                   NVGcolor stroke) {
    // Cloud silhouette with the Quark "Q" ring inside, matching the app icon.
    drawCloud(vg, cx, cy - 6 * s, s * 1.2f, fill);

    nvgBeginPath(vg);
    nvgCircle(vg, cx + 12 * s, cy + 4 * s, 17 * s);
    nvgStrokeColor(vg, stroke);
    nvgStrokeWidth(vg, 4 * s);
    nvgStroke(vg);

    nvgBeginPath(vg);
    nvgMoveTo(vg, cx + 24 * s, cy + 16 * s);
    nvgLineTo(vg, cx + 33 * s, cy + 25 * s);
    nvgLineCap(vg, NVG_ROUND);
    nvgStrokeColor(vg, stroke);
    nvgStrokeWidth(vg, 4 * s);
    nvgStroke(vg);
}

void drawSparkle(NVGcontext* vg, float x, float y, float r, NVGcolor color) {
    nvgBeginPath(vg);
    nvgMoveTo(vg, x, y - r);
    nvgLineTo(vg, x + r * 0.35f, y - r * 0.35f);
    nvgLineTo(vg, x + r, y);
    nvgLineTo(vg, x + r * 0.35f, y + r * 0.35f);
    nvgLineTo(vg, x, y + r);
    nvgLineTo(vg, x - r * 0.35f, y + r * 0.35f);
    nvgLineTo(vg, x - r, y);
    nvgLineTo(vg, x - r * 0.35f, y - r * 0.35f);
    nvgClosePath(vg);
    nvgFillColor(vg, color);
    nvgFill(vg);
}

void drawLock(NVGcontext* vg, float cx, float cy, float s, NVGcolor fill,
              NVGcolor stroke) {
    // Shackle: upper half-circle.
    nvgBeginPath(vg);
    nvgArc(vg, cx, cy - 2 * s, 10 * s, kPi, kPi * 2, NVG_CW);
    nvgStrokeColor(vg, stroke);
    nvgStrokeWidth(vg, 2.5f * s);
    nvgStroke(vg);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, cx - 16 * s, cy - 2 * s, 32 * s, 26 * s, 6 * s);
    nvgFillColor(vg, fill);
    nvgFill(vg);
    nvgStrokeColor(vg, stroke);
    nvgStrokeWidth(vg, 1.5f * s);
    nvgStroke(vg);

    // Keyhole.
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy + 8 * s, 3.5f * s);
    nvgFillColor(vg, stroke);
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRect(vg, cx - 1.2f * s, cy + 10 * s, 2.4f * s, 5 * s);
    nvgFill(vg);
}

void drawWarning(NVGcontext* vg, float cx, float cy, float s, NVGcolor color) {
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, 15 * s);
    nvgStrokeColor(vg, color);
    nvgStrokeWidth(vg, 2 * s);
    nvgStroke(vg);

    nvgBeginPath(vg);
    nvgMoveTo(vg, cx, cy - 7 * s);
    nvgLineTo(vg, cx, cy + 1 * s);
    nvgStrokeColor(vg, color);
    nvgStrokeWidth(vg, 2 * s);
    nvgStroke(vg);

    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy + 6.5f * s, 1.8f * s);
    nvgFillColor(vg, color);
    nvgFill(vg);
}

}  // namespace

// -----------------------------------------------------------------------------
// CloudEmptyArt
// -----------------------------------------------------------------------------

class CloudEmptyArt : public brls::View {
public:
    void setMode(CloudEmptyView::Mode value) { this->mode = value; }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (width <= 0 || height <= 0) return;

        brls::Theme theme = brls::Application::getTheme();
        NVGcolor accent = this->a(theme["brls/accent"]);

        float cx = x + width * 0.5f;
        float cy = y + height * 0.5f;
        float s = std::min(width / 180.0f, height / 150.0f);

        // Soft accent halo behind the illustration.
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy + 6 * s, 66 * s);
        nvgFillColor(vg, fade(accent, 0.10f));
        nvgFill(vg);

        NVGcolor tint = fade(accent, 0.24f);
        NVGcolor strong = accent;
        strong.a = std::min(1.0f, accent.a * 0.95f);

        switch (this->mode) {
            case CloudEmptyView::Mode::Empty:
                drawQuarkMark(vg, cx, cy + 4 * s, s, tint, strong);
                break;

            case CloudEmptyView::Mode::Login:
                drawCloud(vg, cx, cy - 20 * s, s, tint);
                drawLock(vg, cx, cy + 22 * s, s, tint, strong);
                drawSparkle(vg, cx + 54 * s, cy - 34 * s, 7 * s,
                            fade(accent, 0.5f));
                break;

            case CloudEmptyView::Mode::Error:
                drawCloud(vg, cx, cy - 22 * s, s, tint);
                drawWarning(vg, cx, cy + 26 * s, s * 1.15f, strong);
                // Broken connection dashes around the warning.
                nvgBeginPath(vg);
                nvgCircle(vg, cx, cy + 26 * s, 22 * s);
                nvgStrokeColor(vg, fade(accent, 0.2f));
                nvgStrokeWidth(vg, 1.2f * s);
                nvgStroke(vg);
                break;
        }
    }

private:
    CloudEmptyView::Mode mode = CloudEmptyView::Mode::Empty;
};

// -----------------------------------------------------------------------------
// CloudEmptyView
// -----------------------------------------------------------------------------

CloudEmptyView::CloudEmptyView() {
    brls::Theme theme = brls::Application::getTheme();

    this->setAxis(brls::Axis::COLUMN);
    this->setGrow(1);
    this->setJustifyContent(brls::JustifyContent::CENTER);
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setPadding(16, 32, 32, 32);

    this->art = new CloudEmptyArt();
    this->art->setDimensions(180, 150);
    this->art->setMarginBottom(2);
    this->addView(this->art);

    this->titleLabel = new brls::Label();
    this->titleLabel->setFontSize(24);
    this->titleLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    this->addView(this->titleLabel);

    this->detailLabel = new brls::Label();
    this->detailLabel->setWidth(560);
    this->detailLabel->setFontSize(15);
    this->detailLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    this->detailLabel->setTextColor(theme["font/grey"]);
    this->detailLabel->setMarginTop(10);
    this->addView(this->detailLabel);

    this->actionButton = new brls::Button();
    this->actionButton->setStyle(&brls::BUTTONSTYLE_HIGHLIGHT);
    this->actionButton->setTextColor(theme["brls/accent"]);
    this->actionButton->setWidth(220);
    this->actionButton->setMarginTop(22);
    this->actionButton->setVisibility(brls::Visibility::GONE);
    this->actionButton->registerClickAction([this](brls::View*) {
        if (this->actionCallback) this->actionCallback();
        return true;
    });
    this->addView(this->actionButton);
}

void CloudEmptyView::setContent(Mode mode, const std::string& title,
                                const std::string& detail) {
    this->art->setMode(mode);
    this->titleLabel->setText(title);
    this->detailLabel->setText(detail);
}

void CloudEmptyView::setAction(const std::string& label,
                               std::function<void()> callback) {
    this->actionCallback = std::move(callback);
    if (label.empty()) {
        this->actionButton->setVisibility(brls::Visibility::GONE);
        return;
    }
    this->actionButton->setText(label);
    this->actionButton->setVisibility(brls::Visibility::VISIBLE);
}

brls::View* CloudEmptyView::focusTarget() {
    if (this->actionButton->getVisibility() != brls::Visibility::VISIBLE)
        return nullptr;
    return this->actionButton;
}

}  // namespace cloud
