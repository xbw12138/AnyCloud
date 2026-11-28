//
// cloud: feature tour and about pages.
//

#include "cloud/pages/info_page.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <borealis/core/thread.hpp>
#include <borealis/views/image.hpp>

#ifndef NXKIT_APP_VERSION
#define NXKIT_APP_VERSION "unknown"
#endif

using namespace brls::literals;

namespace cloud {

namespace {

enum class Glyph {
    Cloud,
    Qr,
    Folder,
    Edit,
    Play,
    Speed,
    Clock,
    Caption,
    Layers,
    Photo,
    Grid,
    List,
    Download,
    Upload,
    Document,
    Account,
};

struct Feature {
    Glyph glyph;
    NVGcolor tint;
    const char* titleKey;
    const char* detailKey;
};

NVGcolor withAlpha(NVGcolor color, float alpha) {
    return nvgRGBAf(color.r, color.g, color.b, alpha);
}

void strokeSetup(NVGcontext* vg, NVGcolor color, float width) {
    nvgStrokeColor(vg, color);
    nvgStrokeWidth(vg, width);
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
}

/// Draws `glyph` centred at (cx, cy) inside a box of side `s`. Everything is
/// drawn with NanoVG so no icon font has to contain the right codepoint.
void drawGlyph(NVGcontext* vg, Glyph glyph, float cx, float cy, float s,
               NVGcolor color) {
    if (s <= 0) return;

    switch (glyph) {
        case Glyph::Cloud:
            nvgBeginPath(vg);
            nvgCircle(vg, cx - 0.19f * s, cy + 0.05f * s, 0.19f * s);
            nvgCircle(vg, cx + 0.02f * s, cy - 0.08f * s, 0.25f * s);
            nvgCircle(vg, cx + 0.23f * s, cy + 0.05f * s, 0.17f * s);
            nvgRect(vg, cx - 0.38f * s, cy + 0.05f * s, 0.61f * s, 0.19f * s);
            nvgFillColor(vg, color);
            nvgFill(vg);
            break;

        case Glyph::Qr: {
            float q = 0.17f * s;
            strokeSetup(vg, color, 0.075f * s);
            const float spots[3][2] = {
                {-0.36f, -0.36f}, {0.19f, -0.36f}, {-0.36f, 0.19f}};
            for (const auto& spot : spots) {
                nvgBeginPath(vg);
                nvgRoundedRect(vg, cx + spot[0] * s, cy + spot[1] * s, q, q,
                               0.03f * s);
                nvgStroke(vg);
            }
            nvgBeginPath(vg);
            nvgRect(vg, cx + 0.20f * s, cy + 0.20f * s, 0.13f * s,
                    0.13f * s);
            nvgFillColor(vg, color);
            nvgFill(vg);
            break;
        }

        case Glyph::Folder:
            nvgBeginPath(vg);
            nvgRoundedRect(vg, cx - 0.36f * s, cy - 0.20f * s, 0.28f * s,
                           0.13f * s, 0.04f * s);
            nvgRoundedRect(vg, cx - 0.36f * s, cy - 0.12f * s, 0.72f * s,
                           0.34f * s, 0.07f * s);
            nvgFillColor(vg, color);
            nvgFill(vg);
            break;

        case Glyph::Edit:
            nvgSave(vg);
            nvgTranslate(vg, cx, cy);
            nvgRotate(vg, -0.785398f);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, -0.28f * s, -0.09f * s, 0.44f * s, 0.18f * s,
                           0.04f * s);
            nvgFillColor(vg, color);
            nvgFill(vg);
            nvgBeginPath(vg);
            nvgMoveTo(vg, 0.17f * s, -0.09f * s);
            nvgLineTo(vg, 0.34f * s, 0);
            nvgLineTo(vg, 0.17f * s, 0.09f * s);
            nvgClosePath(vg);
            nvgFillColor(vg, withAlpha(color, 0.55f));
            nvgFill(vg);
            nvgRestore(vg);
            break;

        case Glyph::Play:
            strokeSetup(vg, color, 0.09f * s);
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx - 0.18f * s, cy - 0.28f * s);
            nvgLineTo(vg, cx - 0.18f * s, cy + 0.28f * s);
            nvgLineTo(vg, cx + 0.30f * s, cy);
            nvgClosePath(vg);
            nvgFillColor(vg, color);
            nvgFill(vg);
            nvgStroke(vg);
            break;

        case Glyph::Speed:
            strokeSetup(vg, color, 0.09f * s);
            for (int i = 0; i < 2; i++) {
                float dx = (i == 0 ? -0.20f : 0.08f) * s;
                nvgBeginPath(vg);
                nvgMoveTo(vg, cx + dx - 0.10f * s, cy - 0.22f * s);
                nvgLineTo(vg, cx + dx + 0.10f * s, cy);
                nvgLineTo(vg, cx + dx - 0.10f * s, cy + 0.22f * s);
                nvgStroke(vg);
            }
            break;

        case Glyph::Clock:
            strokeSetup(vg, color, 0.08f * s);
            nvgBeginPath(vg);
            nvgCircle(vg, cx, cy, 0.32f * s);
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx, cy - 0.17f * s);
            nvgLineTo(vg, cx, cy);
            nvgLineTo(vg, cx + 0.14f * s, cy + 0.07f * s);
            nvgStroke(vg);
            break;

        case Glyph::Caption:
            strokeSetup(vg, color, 0.07f * s);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, cx - 0.34f * s, cy - 0.26f * s, 0.68f * s,
                           0.52f * s, 0.08f * s);
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, cx - 0.20f * s, cy + 0.06f * s, 0.24f * s,
                           0.10f * s, 0.03f * s);
            nvgFillColor(vg, color);
            nvgFill(vg);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, cx + 0.08f * s, cy + 0.06f * s, 0.12f * s,
                           0.10f * s, 0.03f * s);
            nvgFillColor(vg, withAlpha(color, 0.55f));
            nvgFill(vg);
            break;

        case Glyph::Layers:
            strokeSetup(vg, color, 0.07f * s);
            for (int i = 0; i < 3; i++) {
                float top = cy - 0.24f * s + (float)i * 0.17f * s;
                nvgBeginPath(vg);
                nvgRoundedRect(vg, cx - 0.30f * s, top, 0.60f * s, 0.13f * s,
                               0.06f * s);
                nvgStroke(vg);
            }
            break;

        case Glyph::Photo:
            strokeSetup(vg, color, 0.07f * s);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, cx - 0.32f * s, cy - 0.26f * s, 0.64f * s,
                           0.52f * s, 0.07f * s);
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgCircle(vg, cx - 0.13f * s, cy - 0.10f * s, 0.06f * s);
            nvgFillColor(vg, color);
            nvgFill(vg);
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx - 0.26f * s, cy + 0.20f * s);
            nvgLineTo(vg, cx - 0.06f * s, cy - 0.02f * s);
            nvgLineTo(vg, cx + 0.12f * s, cy + 0.20f * s);
            nvgStroke(vg);
            break;

        case Glyph::Grid: {
            float block = 0.21f * s;
            float gap = 0.08f * s;
            nvgStrokeWidth(vg, 0.06f * s);
            nvgStrokeColor(vg, color);
            for (int r = 0; r < 2; r++) {
                for (int c = 0; c < 2; c++) {
                    float bx = cx - block - gap * 0.5f +
                               (float)c * (block + gap);
                    float by = cy - block - gap * 0.5f +
                               (float)r * (block + gap);
                    nvgBeginPath(vg);
                    nvgRoundedRect(vg, bx, by, block, block, 0.05f * s);
                    if (r == 0 && c == 0) {
                        nvgFillColor(vg, color);
                        nvgFill(vg);
                    } else {
                        nvgStroke(vg);
                    }
                }
            }
            break;
        }

        case Glyph::List:
            strokeSetup(vg, color, 0.07f * s);
            for (int i = 0; i < 3; i++) {
                float y0 = cy - 0.20f * s + (float)i * 0.20f * s;
                nvgBeginPath(vg);
                nvgCircle(vg, cx - 0.28f * s, y0, 0.05f * s);
                nvgFillColor(vg, color);
                nvgFill(vg);
                nvgBeginPath(vg);
                nvgMoveTo(vg, cx - 0.14f * s, y0);
                nvgLineTo(vg, cx + 0.30f * s, y0);
                nvgStroke(vg);
            }
            break;

        case Glyph::Download:
            strokeSetup(vg, color, 0.07f * s);
            // Arrow pointing into a tray.
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx, cy - 0.30f * s);
            nvgLineTo(vg, cx, cy + 0.02f * s);
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx - 0.15f * s, cy - 0.11f * s);
            nvgLineTo(vg, cx, cy + 0.04f * s);
            nvgLineTo(vg, cx + 0.15f * s, cy - 0.11f * s);
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx - 0.30f * s, cy + 0.12f * s);
            nvgLineTo(vg, cx - 0.30f * s, cy + 0.26f * s);
            nvgLineTo(vg, cx + 0.30f * s, cy + 0.26f * s);
            nvgLineTo(vg, cx + 0.30f * s, cy + 0.12f * s);
            nvgStroke(vg);
            break;

        case Glyph::Upload:
            strokeSetup(vg, color, 0.07f * s);
            // Same arrow as Download, pointing out of the tray.
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx, cy + 0.02f * s);
            nvgLineTo(vg, cx, cy - 0.30f * s);
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx - 0.15f * s, cy - 0.11f * s);
            nvgLineTo(vg, cx, cy - 0.30f * s);
            nvgLineTo(vg, cx + 0.15f * s, cy - 0.11f * s);
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx - 0.30f * s, cy + 0.12f * s);
            nvgLineTo(vg, cx - 0.30f * s, cy + 0.26f * s);
            nvgLineTo(vg, cx + 0.30f * s, cy + 0.26f * s);
            nvgLineTo(vg, cx + 0.30f * s, cy + 0.12f * s);
            nvgStroke(vg);
            break;

        case Glyph::Document:
            strokeSetup(vg, color, 0.07f * s);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, cx - 0.22f * s, cy - 0.30f * s, 0.44f * s,
                           0.60f * s, 0.06f * s);
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx - 0.10f * s, cy - 0.12f * s);
            nvgLineTo(vg, cx + 0.10f * s, cy - 0.12f * s);
            nvgMoveTo(vg, cx - 0.10f * s, cy - 0.01f * s);
            nvgLineTo(vg, cx + 0.10f * s, cy - 0.01f * s);
            nvgMoveTo(vg, cx - 0.10f * s, cy + 0.10f * s);
            nvgLineTo(vg, cx + 0.02f * s, cy + 0.10f * s);
            nvgStroke(vg);
            break;

        case Glyph::Account:
            strokeSetup(vg, color, 0.07f * s);
            nvgBeginPath(vg);
            nvgCircle(vg, cx, cy - 0.14f * s, 0.13f * s);
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgArc(vg, cx, cy + 0.14f * s, 0.26f * s, 3.14159265f, 6.28318531f,
                   NVG_CW);
            nvgStroke(vg);
            break;
    }
}

class GlyphView : public brls::View {
public:
    GlyphView(Glyph glyph, NVGcolor color)
        : glyph(glyph), color(color) {}

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (width <= 0 || height <= 0) return;
        drawGlyph(vg, this->glyph, x + width * 0.5f, y + height * 0.5f,
                  std::min(width, height), this->color);
    }

private:
    Glyph glyph;
    NVGcolor color;
};

/// Feature card: tinted glyph tile, title and one-line description.
class FeatureCard : public brls::Box {
public:
    FeatureCard(Glyph glyph, NVGcolor tint, const std::string& title,
                const std::string& detail) {
        brls::Theme theme = brls::Application::getTheme();

        this->setAxis(brls::Axis::ROW);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setHeight(92);
        this->setPadding(0, 18, 0, 18);
        this->setCornerRadius(18);
        this->setHighlightCornerRadius(18);
        this->setBorderColor(theme["brls/accent"]);
        this->setBorderThickness(0);
        this->setBackgroundColor(theme["cloud/surface"]);
        this->setFocusable(true);

        brls::Box* tile = new brls::Box(brls::Axis::COLUMN);
        tile->setDimensions(46, 46);
        tile->setCornerRadius(14);
        tile->setAlignItems(brls::AlignItems::CENTER);
        tile->setJustifyContent(brls::JustifyContent::CENTER);
        tile->setBackgroundColor(withAlpha(tint, 0.18f));
        tile->setMarginRight(16);

        GlyphView* icon = new GlyphView(glyph, tint);
        icon->setDimensions(26, 26);
        tile->addView(icon);
        this->addView(tile);

        brls::Box* text = new brls::Box(brls::Axis::COLUMN);
        text->setGrow(1);
        text->setJustifyContent(brls::JustifyContent::CENTER);

        brls::Label* titleLabel = new brls::Label();
        titleLabel->setText(title);
        titleLabel->setFontSize(17);
        titleLabel->setTextColor(theme["cloud/text"]);
        titleLabel->setHeight(22);
        titleLabel->setSingleLine(true);
        text->addView(titleLabel);

        brls::Label* detailLabel = new brls::Label();
        detailLabel->setText(detail);
        detailLabel->setFontSize(13);
        detailLabel->setTextColor(theme["cloud/text_secondary"]);
        detailLabel->setHeight(18);
        detailLabel->setMarginTop(4);
        detailLabel->setSingleLine(true);
        text->addView(detailLabel);

        this->addView(text);
    }

    void onFocusGained() override {
        brls::Box::onFocusGained();
        this->refreshStyle(true);
    }

    void onFocusLost() override {
        brls::Box::onFocusLost();
        this->refreshStyle(false);
    }

private:
    void refreshStyle(bool focused) {
        brls::Theme theme = brls::Application::getTheme();
        this->setBackgroundColor(focused ? theme["cloud/surface_alt"]
                                         : theme["cloud/surface"]);
        this->setBorderThickness(focused ? 2.0f : 0.0f);
    }
};

/// Accent gradient banner used as the header of the about page.
class HeroCard : public brls::Box {
public:
    using brls::Box::Box;

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (width > 0 && height > 0) {
            brls::Theme theme = brls::Application::getTheme();
            NVGcolor accent = theme["brls/accent"];
            NVGcolor deep = nvgRGBf(accent.r * 0.42f, accent.g * 0.42f,
                                    accent.b * 0.58f);

            NVGpaint paint = nvgLinearGradient(vg, x, y, x + width, y + height,
                                               accent, deep);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width, height, 24);
            nvgFillPaint(vg, paint);
            nvgFill(vg);

            NVGpaint glow = nvgRadialGradient(
                vg, x + width * 0.84f, y + height * 0.16f, 8.0f,
                std::max(width, height) * 0.7f, nvgRGBAf(1, 1, 1, 0.30f),
                nvgRGBAf(1, 1, 1, 0));
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width, height, 24);
            nvgFillPaint(vg, glow);
            nvgFill(vg);
        }
        brls::Box::draw(vg, x, y, width, height, style, ctx);
    }
};

const std::vector<Feature>& features() {
    static const std::vector<Feature> list = {
        {Glyph::Cloud, nvgRGB(86, 156, 255), "cloud/profile/feature/drives/title",
         "cloud/profile/feature/drives/detail"},
        {Glyph::Qr, nvgRGB(150, 120, 255), "cloud/profile/feature/login/title",
         "cloud/profile/feature/login/detail"},
        {Glyph::Folder, nvgRGB(0, 166, 118), "cloud/profile/feature/browse/title",
         "cloud/profile/feature/browse/detail"},
        {Glyph::Edit, nvgRGB(255, 170, 60), "cloud/profile/feature/manage/title",
         "cloud/profile/feature/manage/detail"},
        {Glyph::Play, nvgRGB(255, 132, 76), "cloud/profile/feature/play/title",
         "cloud/profile/feature/play/detail"},
        {Glyph::Speed, nvgRGB(255, 96, 140), "cloud/profile/feature/speed/title",
         "cloud/profile/feature/speed/detail"},
        {Glyph::Clock, nvgRGB(64, 150, 255), "cloud/profile/feature/resume/title",
         "cloud/profile/feature/resume/detail"},
        {Glyph::Caption, nvgRGB(120, 190, 110), "cloud/profile/feature/subtitle/title",
         "cloud/profile/feature/subtitle/detail"},
        {Glyph::Layers, nvgRGB(150, 120, 255), "cloud/profile/feature/quality/title",
         "cloud/profile/feature/quality/detail"},
        {Glyph::Photo, nvgRGB(64, 150, 255), "cloud/profile/feature/photo/title",
         "cloud/profile/feature/photo/detail"},
        {Glyph::Document, nvgRGB(86, 156, 255),
         "cloud/profile/feature/document/title",
         "cloud/profile/feature/document/detail"},
        {Glyph::Download, nvgRGB(0, 166, 118),
         "cloud/profile/feature/download/title",
         "cloud/profile/feature/download/detail"},
        {Glyph::Upload, nvgRGB(255, 132, 76),
         "cloud/profile/feature/upload/title",
         "cloud/profile/feature/upload/detail"},
        {Glyph::Account, nvgRGB(255, 170, 60),
         "cloud/profile/feature/account/title",
         "cloud/profile/feature/account/detail"},
        {Glyph::Grid, nvgRGB(0, 166, 178), "cloud/profile/feature/cache/title",
         "cloud/profile/feature/cache/detail"},
        {Glyph::List, nvgRGB(140, 150, 162), "cloud/profile/feature/log/title",
         "cloud/profile/feature/log/detail"},
    };
    return list;
}

/// One line of the key guide: the buttons on the left, what they do on the
/// right. The button names are literal glyphs, so only the description is
/// translated.
struct ControlRow {
    const char* keys;
    const char* detailKey;
};

struct ControlGroup {
    const char* titleKey;
    std::vector<ControlRow> rows;
};

const std::vector<ControlGroup>& controlGroups() {
    static const std::vector<ControlGroup> groups = {
        {"cloud/profile/keyguide/global_title",
         {
             {"十字键 / 左摇杆", "cloud/profile/keyguide/global_move"},
             {"A", "cloud/profile/keyguide/global_ok"},
             {"B", "cloud/profile/keyguide/global_back"},
             {"L / R", "cloud/profile/keyguide/global_tab"},
         }},
        {"cloud/profile/keyguide/files_title",
         {
             {"A", "cloud/profile/keyguide/files_open"},
             {"B", "cloud/profile/keyguide/files_back"},
             {"X", "cloud/profile/keyguide/files_more"},
             {"Y", "cloud/profile/keyguide/files_sort"},
         }},
        {"cloud/profile/keyguide/downloads_title",
         {
             {"Y", "cloud/profile/keyguide/downloads_select"},
             {"X", "cloud/profile/keyguide/downloads_delete"},
             {"A", "cloud/profile/keyguide/downloads_detail"},
         }},
        {"cloud/profile/keyguide/player_title",
         {
             {"A", "cloud/profile/keyguide/player_toggle"},
             {"← / →", "cloud/profile/keyguide/player_seek10"},
             {"↑ / ↓", "cloud/profile/keyguide/player_volume"},
             {"ZL / ZR", "cloud/profile/keyguide/player_seek30"},
             {"L / R", "cloud/profile/keyguide/player_episode"},
             {"X", "cloud/profile/keyguide/player_loop"},
             {"Y", "cloud/profile/keyguide/player_quality"},
             {"-", "cloud/profile/keyguide/player_subtitle"},
             {"+", "cloud/profile/keyguide/player_playlist"},
             {"R3", "cloud/profile/keyguide/player_aspect"},
             {"L3", "cloud/profile/keyguide/player_speed"},
             {"B", "cloud/profile/keyguide/player_exit"},
         }},
        {"cloud/profile/keyguide/image_title",
         {
             {"L / R", "cloud/profile/keyguide/image_page"},
             {"B", "cloud/profile/keyguide/image_back"},
         }},
        {"cloud/profile/keyguide/document_title",
         {
             {"L / R", "cloud/profile/keyguide/document_page"},
             {"十字键", "cloud/profile/keyguide/document_move"},
             {"Y / X", "cloud/profile/keyguide/document_zoom"},
             {"ZL / ZR", "cloud/profile/keyguide/document_chapter"},
             {"B", "cloud/profile/keyguide/document_back"},
         }},
        {"cloud/profile/keyguide/log_title",
         {
             {"A", "cloud/profile/keyguide/log_open"},
             {"X", "cloud/profile/keyguide/log_refresh"},
             {"Y", "cloud/profile/keyguide/log_clear"},
             {"B", "cloud/profile/keyguide/log_back"},
         }},
    };
    return groups;
}

/// Key guide card. It is focusable without a highlight so the scrolling frame
/// can scroll it into view while the D-pad walks down the page.
brls::View* buildControlGroup(const ControlGroup& group) {
    brls::Theme theme = brls::Application::getTheme();

    brls::Box* card = new brls::Box(brls::Axis::COLUMN);
    card->setMarginTop(10);
    card->setPadding(14, 20, 14, 20);
    card->setCornerRadius(18);
    card->setBackgroundColor(theme["cloud/surface"]);
    card->setFocusable(true);
    card->setHideHighlight(true);

    brls::Label* title = new brls::Label();
    title->setText(brls::getStr(group.titleKey));
    title->setFontSize(16);
    title->setTextColor(theme["cloud/text"]);
    title->setHeight(22);
    title->setMarginBottom(4);
    card->addView(title);

    for (const ControlRow& row : group.rows) {
        brls::Box* line = new brls::Box(brls::Axis::ROW);
        line->setAlignItems(brls::AlignItems::CENTER);
        line->setHeight(32);

        brls::Label* keys = new brls::Label();
        keys->setText(row.keys);
        keys->setFontSize(13);
        keys->setTextColor(theme["cloud/text_secondary"]);
        keys->setWidth(170);
        keys->setHeight(20);
        keys->setSingleLine(true);
        line->addView(keys);

        brls::Label* detail = new brls::Label();
        detail->setText(brls::getStr(row.detailKey));
        detail->setFontSize(14);
        detail->setTextColor(theme["cloud/text"]);
        detail->setHeight(20);
        detail->setGrow(1);
        detail->setSingleLine(true);
        line->addView(detail);

        card->addView(line);
    }

    return card;
}

brls::View* buildFeatures(brls::View** firstCard) {
    brls::Theme theme = brls::Application::getTheme();

    brls::ScrollingFrame* scroller = new brls::ScrollingFrame();
    scroller->setGrow(1);
    scroller->setMarginTop(14);
    scroller->setHideHighlight(true);

    brls::Box* content = new brls::Box(brls::Axis::COLUMN);
    content->setPaddingRight(6);
    // Rows must all take the full width, otherwise each row is only as wide as
    // its own text and the two columns look ragged.
    content->setAlignItems(brls::AlignItems::STRETCH);

    brls::Label* intro = new brls::Label();
    intro->setText("cloud/profile/features_intro"_i18n);
    intro->setFontSize(14);
    intro->setTextColor(theme["cloud/text_secondary"]);
    intro->setWidthPercentage(100);
    intro->setSingleLine(false);
    content->addView(intro);

    const auto& list = features();
    const float gap = 14;
    for (size_t i = 0; i < list.size(); i += 2) {
        brls::Box* row = new brls::Box(brls::Axis::ROW);
        row->setMarginTop(i == 0 ? 16.0f : gap);
        row->setHeight(92);
        row->setWidthPercentage(100);

        for (size_t j = 0; j < 2; j++) {
            if (j > 0) {
                brls::Box* spacer = new brls::Box();
                spacer->setWidth((int)gap);
                spacer->setHeight(1);
                row->addView(spacer);
            }
            size_t index = i + j;
            if (index >= list.size()) {
                brls::Box* filler = new brls::Box();
                filler->setGrow(1);
                filler->setHeight(1);
                row->addView(filler);
                continue;
            }
            const Feature& feature = list[index];
            FeatureCard* card =
                new FeatureCard(feature.glyph, feature.tint,
                                brls::getStr(feature.titleKey),
                                brls::getStr(feature.detailKey));
            // Zero basis: with auto basis each card starts at its own text
            // width, so equal grow values still produce unequal cards.
            card->setWidth(0);
            card->setGrow(1);
            row->addView(card);
            if (!*firstCard) *firstCard = card;
        }
        content->addView(row);
    }

    brls::Box* bottomGap = new brls::Box();
    bottomGap->setHeight(10);
    content->addView(bottomGap);

    scroller->setContentView(content);
    return scroller;
}

/// Standalone key guide page: one card per context, listing every button.
brls::View* buildControls(brls::View** focusTarget) {
    brls::Theme theme = brls::Application::getTheme();

    brls::ScrollingFrame* scroller = new brls::ScrollingFrame();
    scroller->setGrow(1);
    scroller->setMarginTop(14);
    scroller->setHideHighlight(true);

    brls::Box* content = new brls::Box(brls::Axis::COLUMN);
    content->setPaddingRight(6);
    content->setAlignItems(brls::AlignItems::STRETCH);

    brls::Label* intro = new brls::Label();
    intro->setText("cloud/profile/controls_intro"_i18n);
    intro->setFontSize(14);
    intro->setTextColor(theme["cloud/text_secondary"]);
    intro->setWidthPercentage(100);
    intro->setSingleLine(false);
    content->addView(intro);

    for (const ControlGroup& group : controlGroups()) {
        brls::View* card = buildControlGroup(group);
        content->addView(card);
        if (!*focusTarget) *focusTarget = card;
    }

    brls::Box* bottomGap = new brls::Box();
    bottomGap->setHeight(10);
    content->addView(bottomGap);

    scroller->setContentView(content);
    return scroller;
}

brls::View* infoRow(const std::string& name, const std::string& value) {
    brls::Theme theme = brls::Application::getTheme();

    brls::Box* row = new brls::Box(brls::Axis::ROW);
    row->setHeight(46);
    row->setAlignItems(brls::AlignItems::CENTER);

    brls::Label* key = new brls::Label();
    key->setText(name);
    key->setFontSize(14);
    key->setTextColor(theme["cloud/text_secondary"]);
    key->setWidth(150);
    key->setHeight(20);
    key->setSingleLine(true);
    row->addView(key);

    brls::Label* val = new brls::Label();
    val->setText(value);
    val->setFontSize(15);
    val->setTextColor(theme["cloud/text"]);
    val->setHeight(20);
    val->setGrow(1);
    val->setSingleLine(true);
    row->addView(val);

    return row;
}

brls::View* divider() {
    brls::Theme theme = brls::Application::getTheme();
    brls::Box* line = new brls::Box();
    line->setHeight(1);
    line->setBackgroundColor(theme["cloud/border"]);
    return line;
}

brls::View* buildAbout(brls::View** focusTarget) {
    brls::Theme theme = brls::Application::getTheme();

    brls::ScrollingFrame* scroller = new brls::ScrollingFrame();
    scroller->setGrow(1);
    scroller->setMarginTop(14);
    scroller->setHideHighlight(true);
    scroller->setFocusable(true);

    brls::Box* content = new brls::Box(brls::Axis::COLUMN);
    content->setPaddingRight(6);

    // --- Hero ---------------------------------------------------------------
    HeroCard* hero = new HeroCard(brls::Axis::ROW);
    hero->setHeight(150);
    hero->setPadding(28, 30, 28, 30);
    hero->setAlignItems(brls::AlignItems::CENTER);

    GlyphView* logo = new GlyphView(Glyph::Cloud, nvgRGBAf(1, 1, 1, 0.95f));
    logo->setDimensions(64, 64);
    logo->setMarginRight(24);
    hero->addView(logo);

    brls::Box* nameColumn = new brls::Box(brls::Axis::COLUMN);
    nameColumn->setGrow(1);
    nameColumn->setJustifyContent(brls::JustifyContent::CENTER);

    brls::Label* name = new brls::Label();
    name->setText("AnyCloud");
    name->setFontSize(34);
    name->setTextColor(nvgRGB(255, 255, 255));
    name->setHeight(42);
    name->setSingleLine(true);
    nameColumn->addView(name);

    brls::Label* tagline = new brls::Label();
    tagline->setText("cloud/profile/about_tagline"_i18n);
    tagline->setFontSize(14);
    tagline->setTextColor(nvgRGBAf(1, 1, 1, 0.82f));
    tagline->setHeight(20);
    tagline->setMarginTop(4);
    tagline->setSingleLine(true);
    nameColumn->addView(tagline);
    hero->addView(nameColumn);

    brls::Box* versionPill = new brls::Box(brls::Axis::ROW);
    versionPill->setHeight(30);
    versionPill->setPadding(0, 14, 0, 14);
    versionPill->setCornerRadius(15);
    versionPill->setAlignItems(brls::AlignItems::CENTER);
    versionPill->setJustifyContent(brls::JustifyContent::CENTER);
    versionPill->setBackgroundColor(nvgRGBAf(1, 1, 1, 0.22f));

    brls::Label* version = new brls::Label();
    version->setText("v" NXKIT_APP_VERSION);
    version->setFontSize(13);
    version->setTextColor(nvgRGB(255, 255, 255));
    version->setHeight(18);
    versionPill->addView(version);
    hero->addView(versionPill);
    content->addView(hero);

    // --- Description --------------------------------------------------------
    brls::Label* intro = new brls::Label();
    intro->setText("cloud/profile/about_text"_i18n);
    intro->setFontSize(14);
    intro->setTextColor(theme["cloud/text_secondary"]);
    intro->setWidthPercentage(100);
    intro->setSingleLine(false);
    intro->setMarginTop(18);
    content->addView(intro);

    // --- Facts --------------------------------------------------------------
    brls::Box* facts = new brls::Box(brls::Axis::COLUMN);
    facts->setMarginTop(16);
    facts->setPadding(6, 22, 6, 22);
    facts->setCornerRadius(20);
    facts->setBackgroundColor(theme["cloud/surface"]);

    facts->addView(infoRow("cloud/profile/about_developer"_i18n, "xbw12138"));
    facts->addView(divider());
    facts->addView(infoRow("cloud/profile/about_stack"_i18n,
                           "NXKit · borealis · libmpv"));
    facts->addView(divider());
    facts->addView(infoRow("cloud/profile/about_drives"_i18n,
                           "cloud/profile/about_drives_value"_i18n));
    facts->addView(divider());
    facts->addView(infoRow("cloud/profile/about_source"_i18n,
                           "github.com/xbw12138/AnyCloud"));
    content->addView(facts);

    // --- Follow author ------------------------------------------------------
    brls::Label* followTitle = new brls::Label();
    followTitle->setText("cloud/profile/about_follow_title"_i18n);
    followTitle->setFontSize(15);
    followTitle->setTextColor(theme["cloud/text_secondary"]);
    followTitle->setHeight(20);
    followTitle->setMarginTop(20);
    content->addView(followTitle);

    brls::Box* followCard = new brls::Box(brls::Axis::ROW);
    followCard->setMarginTop(10);
    followCard->setPadding(16, 20, 16, 20);
    followCard->setCornerRadius(20);
    followCard->setAlignItems(brls::AlignItems::CENTER);
    followCard->setBackgroundColor(theme["cloud/surface"]);

    // White square behind the round QR so its corners blend into the card.
    brls::Box* qrFrame = new brls::Box(brls::Axis::COLUMN);
    qrFrame->setDimensions(116, 116);
    qrFrame->setCornerRadius(18);
    qrFrame->setBackgroundColor(nvgRGB(255, 255, 255));
    qrFrame->setAlignItems(brls::AlignItems::CENTER);
    qrFrame->setJustifyContent(brls::JustifyContent::CENTER);
    qrFrame->setMarginRight(22);

    brls::Image* qr = new brls::Image();
    qr->setImageFromFile("romfs:/pictures/douyin_qr.png");
    qr->setDimensions(110, 110);
    qrFrame->addView(qr);
    followCard->addView(qrFrame);

    brls::Box* followText = new brls::Box(brls::Axis::COLUMN);
    followText->setGrow(1);
    followText->setJustifyContent(brls::JustifyContent::CENTER);

    brls::Label* platform = new brls::Label();
    platform->setText("cloud/profile/about_follow_platform"_i18n);
    platform->setFontSize(12);
    platform->setTextColor(theme["brls/accent"]);
    platform->setHeight(16);
    followText->addView(platform);

    brls::Label* author = new brls::Label();
    author->setText("cloud/profile/about_follow_name"_i18n);
    author->setFontSize(20);
    author->setTextColor(theme["cloud/text"]);
    author->setHeight(26);
    author->setMarginTop(2);
    author->setSingleLine(true);
    followText->addView(author);

    brls::Label* account = new brls::Label();
    account->setText("cloud/profile/about_follow_id"_i18n);
    account->setFontSize(14);
    account->setTextColor(theme["cloud/text_secondary"]);
    account->setHeight(18);
    account->setMarginTop(3);
    account->setSingleLine(true);
    followText->addView(account);

    brls::Label* followHint = new brls::Label();
    followHint->setText("cloud/profile/about_follow_hint"_i18n);
    followHint->setFontSize(13);
    followHint->setTextColor(theme["cloud/text_secondary"]);
    followHint->setHeight(18);
    followHint->setMarginTop(6);
    followHint->setSingleLine(true);
    followText->addView(followHint);

    followCard->addView(followText);
    content->addView(followCard);

    // --- Thanks -------------------------------------------------------------
    brls::Label* thanks = new brls::Label();
    thanks->setText("cloud/profile/about_thanks"_i18n);
    thanks->setFontSize(13);
    thanks->setTextColor(theme["cloud/text_secondary"]);
    thanks->setWidthPercentage(100);
    thanks->setSingleLine(false);
    thanks->setMarginTop(16);
    content->addView(thanks);

    brls::Box* bottomGap = new brls::Box();
    bottomGap->setHeight(10);
    content->addView(bottomGap);

    scroller->setContentView(content);
    // Nothing on this page is interactive, but the activity still needs a
    // focus target so that B (and popActivity) work; the ring is hidden.
    *focusTarget = scroller;
    return scroller;
}

}  // namespace

brls::View* InfoPage::createContentView() {
    brls::Theme theme = brls::Application::getTheme();

    brls::Box* root = new brls::Box(brls::Axis::COLUMN);
    root->setGrow(1);
    root->setPadding(26, 40, 20, 40);
    root->setBackgroundColor(theme["cloud/bg"]);

    brls::Box* header = new brls::Box(brls::Axis::ROW);
    header->setAlignItems(brls::AlignItems::CENTER);

    std::string titleKey = "cloud/profile/about"_i18n;
    std::string hintKey = "cloud/profile/about_hint"_i18n;
    switch (this->section) {
        case Section::Features:
            titleKey = "cloud/profile/features_title"_i18n;
            hintKey = "cloud/profile/features_hint"_i18n;
            break;
        case Section::Controls:
            titleKey = "cloud/profile/controls_title"_i18n;
            hintKey = "cloud/profile/controls_hint"_i18n;
            break;
        default:
            break;
    }

    brls::Label* title = new brls::Label();
    title->setText(titleKey);
    title->setFontSize(28);
    title->setTextColor(theme["cloud/text"]);
    title->setHeight(36);
    header->addView(title);

    brls::Label* hint = new brls::Label();
    hint->setText(hintKey);
    hint->setFontSize(14);
    hint->setTextColor(theme["cloud/text_secondary"]);
    hint->setHeight(20);
    hint->setGrow(1);
    hint->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    header->addView(hint);

    root->addView(header);

    brls::View* focusTarget = nullptr;
    brls::View* body = nullptr;
    switch (this->section) {
        case Section::Features:
            body = buildFeatures(&focusTarget);
            break;
        case Section::Controls:
            body = buildControls(&focusTarget);
            break;
        default:
            body = buildAbout(&focusTarget);
            break;
    }
    root->addView(body);

    // Borealis does not pop activities on B by itself.
    root->registerAction("hints/back"_i18n, brls::BUTTON_B,
                         [](brls::View*) {
                             brls::Application::popActivity();
                             return true;
                         });

    // pushActivity() only focuses the content view itself, not a focusable
    // child, so move the focus into the page once it is laid out. Without this
    // the focus stays on the tile of the hidden page and B never reaches us.
    if (focusTarget) {
        brls::sync([focusTarget] {
            brls::Application::giveFocus(focusTarget);
        });
    }

    return root;
}

}  // namespace cloud
