//
// cloud: download task manager page.
//

#include "cloud/pages/downloads_page.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>

#include "cloud/ui/popup.hpp"
#include "nxkit/ui/auto_tab_frame.hpp"

using namespace brls::literals;

namespace cloud {

namespace {

const int kRefreshPeriodMs = 500;

uint64_t nowMs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch())
        .count();
}

NVGcolor withAlpha(NVGcolor color, float alpha) {
    color.a = alpha;
    return color;
}

std::string formatBytes(uint64_t bytes) {
    char buf[64];
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(buf, sizeof(buf), "%.2f GB",
                 (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        snprintf(buf, sizeof(buf), "%.1f MB",
                 (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(buf, sizeof(buf), "%.0f KB", (double)bytes / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
    }
    return buf;
}

std::string formatDuration(uint64_t seconds) {
    if (seconds < 1) seconds = 1;
    std::string hourUnit = "cloud/download/hour"_i18n;
    std::string minuteUnit = "cloud/download/minute"_i18n;
    std::string secondUnit = "cloud/download/second"_i18n;

    if (seconds >= 3600) {
        std::string out =
            std::to_string(seconds / 3600) + hourUnit;
        uint64_t minutes = (seconds % 3600) / 60;
        if (minutes > 0) out += std::to_string(minutes) + minuteUnit;
        return out;
    }
    if (seconds >= 60) {
        std::string out = std::to_string(seconds / 60) + minuteUnit;
        uint64_t rest = seconds % 60;
        if (rest > 0) out += std::to_string(rest) + secondUnit;
        return out;
    }
    return std::to_string(seconds) + secondUnit;
}

bool isActive(DownloadState state) {
    return state == DownloadState::Queued ||
           state == DownloadState::Running;
}

NVGcolor stateColor(DownloadState state) {
    brls::Theme theme = brls::Application::getTheme();
    switch (state) {
        case DownloadState::Running:
            return theme["brls/accent"];
        case DownloadState::Done:
            return nvgRGB(72, 199, 132);
        case DownloadState::Failed:
            return nvgRGB(236, 96, 96);
        case DownloadState::Queued:
        case DownloadState::Cancelled:
        default:
            return theme["cloud/text_secondary"];
    }
}

std::string stateText(DownloadState state) {
    switch (state) {
        case DownloadState::Queued: return "cloud/download/queued"_i18n;
        case DownloadState::Running: return "cloud/download/running"_i18n;
        case DownloadState::Done: return "cloud/download/done"_i18n;
        case DownloadState::Failed: return "cloud/download/failed"_i18n;
        case DownloadState::Cancelled: return "cloud/download/cancelled"_i18n;
    }
    return "cloud/download/queued"_i18n;
}

/// Labels measure to NaN width when their text is empty, which corrupts the
/// whole row's layout, so empty labels are collapsed instead.
void setLabelText(brls::Label* label, const std::string& text) {
    label->setText(text);
    label->setVisibility(text.empty() ? brls::Visibility::GONE
                                      : brls::Visibility::VISIBLE);
}

/// Joins the non-empty parts with a middle dot separator.
std::string joinMeta(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& part : parts) {
        if (part.empty()) continue;
        if (!out.empty()) out += "  ·  ";
        out += part;
    }
    return out;
}

/// Rounded track with a gradient fill, sized by the caller.
class ProgressBar : public brls::View {
public:
    void setProgress(float value) {
        float next = std::min(1.0f, std::max(0.0f, value));
        if (next == this->progress) return;
        this->progress = next;
        this->invalidate();
    }

    void setTrack(NVGcolor color) { this->trackColor = color; }

    void setFill(NVGcolor start, NVGcolor end) {
        this->fillStart = start;
        this->fillEnd = end;
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (width <= 0 || height <= 0) return;
        float radius = height * 0.5f;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, radius);
        nvgFillColor(vg, this->trackColor);
        nvgFill(vg);

        if (this->progress <= 0.0f) return;
        float fillWidth = std::max(height, width * this->progress);
        NVGpaint paint = nvgLinearGradient(vg, x, y, x + width, y,
                                          this->fillStart, this->fillEnd);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, fillWidth, height, radius);
        nvgFillPaint(vg, paint);
        nvgFill(vg);
    }

private:
    float progress = 0.0f;
    NVGcolor trackColor = nvgRGBA(255, 255, 255, 40);
    NVGcolor fillStart = nvgRGB(255, 255, 255);
    NVGcolor fillEnd = nvgRGBA(255, 255, 255, 215);
};

/// Large outlined arrow used by the empty state.
class EmptyArt : public brls::View {
public:
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (width <= 0 || height <= 0) return;
        brls::Theme theme = brls::Application::getTheme();
        float cx = x + width * 0.5f;
        float cy = y + height * 0.5f;

        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, 38);
        nvgFillColor(vg, theme["cloud/surface"]);
        nvgFill(vg);

        nvgStrokeWidth(vg, 2.5f);
        nvgStrokeColor(vg, withAlpha(theme["brls/accent"], 0.9f));
        nvgLineCap(vg, NVG_ROUND);
        nvgLineJoin(vg, NVG_ROUND);

        nvgBeginPath(vg);
        nvgMoveTo(vg, cx, cy - 16);
        nvgLineTo(vg, cx, cy + 6);
        nvgStroke(vg);

        nvgBeginPath(vg);
        nvgMoveTo(vg, cx - 9, cy - 4);
        nvgLineTo(vg, cx, cy + 7);
        nvgLineTo(vg, cx + 9, cy - 4);
        nvgStroke(vg);

        nvgBeginPath(vg);
        nvgMoveTo(vg, cx - 15, cy + 15);
        nvgLineTo(vg, cx - 15, cy + 24);
        nvgLineTo(vg, cx + 15, cy + 24);
        nvgLineTo(vg, cx + 15, cy + 15);
        nvgStroke(vg);
    }
};


/// One line of "label  value" inside the details card.
void addInfoRow(brls::Box* card, const std::string& key,
                const std::string& value, NVGcolor color) {
    brls::Theme theme = brls::Application::getTheme();

    brls::Box* row = new brls::Box(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setWidthPercentage(100);
    row->setMarginTop(10);

    brls::Label* label = new brls::Label();
    label->setText(key);
    label->setFontSize(12);
    label->setHeight(18);
    label->setWidth(80);
    label->setTextColor(theme["cloud/text_secondary"]);
    row->addView(label);

    brls::Label* text = new brls::Label();
    text->setText(value);
    text->setFontSize(13);
    text->setHeight(18);
    text->setSingleLine(true);
    text->setGrow(1);
    text->setTextColor(color);
    row->addView(text);

    card->addView(row);
}

/// One task card. Highlight is drawn manually because a plain Box does not
/// show focus on its own.
class TaskCard : public brls::Box {
public:
    TaskCard(const std::string& hint, std::function<void()> activate)
        : activate(std::move(activate)) {
        this->setAxis(brls::Axis::COLUMN);
        this->setMarginBottom(10);
        this->setCornerRadius(16);
        this->setHighlightCornerRadius(16);
        this->setBorderColor(brls::Application::getTheme()["brls/accent"]);
        this->setBorderThickness(0);
        this->setFocusable(true);

        this->registerAction(hint, brls::BUTTON_A, [this](brls::View*) {
            if (this->activate) this->activate();
            return true;
        });
        this->registerClickAction([this](brls::View*) {
            if (this->activate) this->activate();
            return true;
        });
        this->refreshStyle();
    }

    void onFocusGained() override {
        brls::Box::onFocusGained();
        this->refreshStyle();
    }

    void onFocusLost() override {
        brls::Box::onFocusLost();
        this->refreshStyle();
    }

    void refreshStyle() {
        brls::Theme theme = brls::Application::getTheme();
        bool focused = this->isFocused();
        this->setBackgroundColor(focused ? theme["cloud/surface_alt"]
                                         : theme["cloud/surface"]);
        this->setBorderThickness(focused ? 2.0f : 0.0f);
    }

private:
    std::function<void()> activate;
};

class RefreshTask : public brls::RepeatingTask {
public:
    explicit RefreshTask(DownloadPage* page)
        : brls::RepeatingTask(kRefreshPeriodMs), page(page) {}

    void run() override { this->page->refresh(); }

private:
    DownloadPage* page;
};

}  // namespace

DownloadPage::~DownloadPage() {
    if (this->timer) {
        this->timer->stop();
        delete this->timer;
        this->timer = nullptr;
    }
}

DownloadPage::DownloadPage() {
    brls::Theme theme = brls::Application::getTheme();

    this->setAxis(brls::Axis::COLUMN);
    this->setGrow(1);
    this->setPadding(24, 48, 20, 48);
    this->setBackgroundColor(theme["cloud/bg"]);

    this->addView(this->buildSelectBar());

    brls::ScrollingFrame* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1);
    this->listBox = new brls::Box(brls::Axis::COLUMN);
    this->listBox->setGrow(1);
    scroll->setContentView(this->listBox);
    this->addView(scroll);

    // Y toggles multi-select; X acts on the selection (or clears finished
    // tasks when nothing is selected).
    this->registerAction("cloud/download/select"_i18n, brls::BUTTON_Y,
                         [this](brls::View*) {
                             this->toggleSelectMode();
                             return true;
                         });
    this->registerAction("cloud/download/delete"_i18n, brls::BUTTON_X,
                         [this](brls::View*) {
                             if (this->selecting && !this->selected.empty())
                                 this->confirmDeleteSelected();
                             else if (!this->selecting)
                                 this->confirmClearFinished();
                             return true;
                         });

    this->timer = new RefreshTask(this);
    this->timer->start();
    this->refresh();
}

/// Slim bar shown only while multi-select is active; the selection count used
/// to live in the removed summary card.
brls::Box* DownloadPage::buildSelectBar() {
    brls::Theme theme = brls::Application::getTheme();

    brls::Box* bar = new brls::Box(brls::Axis::ROW);
    bar->setAlignItems(brls::AlignItems::CENTER);
    bar->setHeight(38);
    bar->setPadding(0, 14, 0, 14);
    bar->setCornerRadius(10);
    bar->setMarginBottom(10);
    bar->setBackgroundColor(theme["cloud/surface_alt"]);
    bar->setVisibility(brls::Visibility::GONE);

    this->selectText = new brls::Label();
    this->selectText->setFontSize(13);
    this->selectText->setHeight(18);
    this->selectText->setTextColor(theme["cloud/text"]);
    bar->addView(this->selectText);

    this->selectBar = bar;
    return bar;
}

void DownloadPage::refresh() {
    std::vector<DownloadTask> tasks = DownloadManager::instance().snapshot();

    bool stale = !this->built || tasks.size() != this->rows.size();
    if (!stale) {
        for (size_t i = 0; i < tasks.size(); i++) {
            const Row* row = nullptr;
            for (const Row& candidate : this->rows)
                if (candidate.id == tasks[i].id) row = &candidate;
            if (!row || row->state != tasks[i].state) {
                stale = true;
                break;
            }
        }
    }

    this->latest = tasks;
    if (stale) this->rebuild();

    uint64_t now = nowMs();
    for (Row& row : this->rows) {
        const DownloadTask* task = this->findTask(row.id);
        if (task) this->updateRow(row, *task, now);
    }
    this->updateSummary();
    this->applyTabRoutes();

    // Never leave the tab without a focus target: navigation would die.
    if (!this->rows.empty() &&
        this->getVisibility() == brls::Visibility::VISIBLE &&
        brls::Application::getCurrentFocus() == nullptr)
        brls::Application::giveFocus(this->rows.front().card);
}

void DownloadPage::applyTabRoutes() {
    brls::View* tabItem = this->activeTabItem();
    if (!tabItem || this->rows.empty()) return;

    for (Row& row : this->rows) {
        if (!row.card) continue;
        // LEFT always escapes to the tab bar; UP does too from the first row,
        // otherwise it steps through the list as usual.
        row.card->setCustomNavigationRoute(brls::FocusDirection::LEFT,
                                           tabItem);
    }
    this->rows.front().card->setCustomNavigationRoute(
        brls::FocusDirection::UP, tabItem);
}

brls::Box* DownloadPage::sectionHeader(const std::string& title, size_t count,
                                       bool spaced,
                                       brls::Label** summaryOut) {
    brls::Theme theme = brls::Application::getTheme();

    brls::Box* header = new brls::Box(brls::Axis::ROW);
    header->setAlignItems(brls::AlignItems::CENTER);
    header->setMarginTop(spaced ? 14 : 2);
    header->setMarginBottom(10);
    header->setMarginLeft(4);

    brls::Box* mark = new brls::Box();
    mark->setWidth(3);
    mark->setHeight(14);
    mark->setCornerRadius(2);
    mark->setMarginRight(8);
    mark->setBackgroundColor(theme["brls/accent"]);
    header->addView(mark);

    brls::Label* label = new brls::Label();
    label->setText(title + "  " + std::to_string(count));
    label->setFontSize(14);
    label->setHeight(20);
    label->setGrow(1);
    label->setTextColor(theme["cloud/text"]);
    header->addView(label);

    if (summaryOut) {
        brls::Label* summary = new brls::Label();
        summary->setFontSize(12);
        summary->setHeight(17);
        summary->setMarginRight(6);
        summary->setTextColor(theme["brls/accent"]);
        summary->setVisibility(brls::Visibility::GONE);
        header->addView(summary);
        *summaryOut = summary;
    }

    return header;
}

void DownloadPage::buildEmptyState() {
    brls::Theme theme = brls::Application::getTheme();

    brls::Box* empty = new brls::Box(brls::Axis::COLUMN);
    empty->setGrow(1);
    empty->setAlignItems(brls::AlignItems::CENTER);
    empty->setJustifyContent(brls::JustifyContent::CENTER);

    EmptyArt* art = new EmptyArt();
    art->setWidth(100);
    art->setHeight(100);
    art->setMarginBottom(18);
    empty->addView(art);

    brls::Label* title = new brls::Label();
    title->setText("cloud/download/empty"_i18n);
    title->setFontSize(19);
    title->setTextColor(theme["cloud/text"]);
    title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    title->setMarginBottom(8);
    empty->addView(title);

    brls::Label* hint = new brls::Label();
    hint->setText("cloud/download/empty_hint"_i18n);
    hint->setFontSize(13);
    hint->setTextColor(theme["cloud/text_secondary"]);
    hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    empty->addView(hint);

    this->listBox->addView(empty);
}

void DownloadPage::rebuild() {
    // Keep the highlight on the same task after a rebuild.
    uint64_t focusedId = 0;
    for (const Row& row : this->rows)
        if (row.card && row.card->isFocused()) focusedId = row.id;

    this->listBox->clearViews();
    this->rows.clear();
    this->activeSummary = nullptr;
    this->built = true;
    uint64_t now = nowMs();

    // Forget selections of tasks that no longer exist.
    this->selected.erase(
        std::remove_if(this->selected.begin(), this->selected.end(),
                       [this](uint64_t id) { return !this->findTask(id); }),
        this->selected.end());
    if (this->latest.empty()) this->selecting = false;

    if (this->latest.empty()) {
        this->buildEmptyState();
        return;
    }

    std::vector<const DownloadTask*> active;
    std::vector<const DownloadTask*> finished;
    for (const auto& task : this->latest) {
        if (isActive(task.state))
            active.push_back(&task);
        else
            finished.push_back(&task);
    }

    if (!active.empty()) {
        this->listBox->addView(this->sectionHeader(
            "cloud/download/section/active"_i18n, active.size(), false,
            &this->activeSummary));
        for (const DownloadTask* task : active)
            this->listBox->addView(this->buildActiveCard(*task, now));
    }
    if (!finished.empty()) {
        this->listBox->addView(this->sectionHeader(
            "cloud/download/section/finished"_i18n, finished.size(),
            !active.empty()));
        for (const DownloadTask* task : finished)
            this->listBox->addView(this->buildFinishedRow(*task, now));
    }

    Row* focusedRow = nullptr;
    for (Row& row : this->rows)
        if (row.id == focusedId) focusedRow = &row;

    if (focusedRow)
        brls::Application::giveFocus(focusedRow->card);
    else if (!this->rows.empty())
        brls::Application::giveFocus(this->rows.front().card);
}

brls::Box* DownloadPage::buildActiveCard(const DownloadTask& task,
                                         uint64_t now) {
    brls::Theme theme = brls::Application::getTheme();

    const std::string hint =
        this->selecting ? "cloud/download/choose"_i18n
                        : "cloud/download/details"_i18n;
    TaskCard* card = new TaskCard(hint, [this, id = task.id] {
        this->runRowAction(id);
    });
    card->setPadding(14, 18, 14, 18);

    brls::Box* top = new brls::Box(brls::Axis::ROW);
    top->setAlignItems(brls::AlignItems::CENTER);

    brls::Label* check = new brls::Label();
    check->setFontSize(15);
    check->setHeight(20);
    check->setMarginRight(10);
    check->setTextColor(theme["brls/accent"]);
    check->setVisibility(this->selecting ? brls::Visibility::VISIBLE
                                         : brls::Visibility::GONE);
    top->addView(check);

    brls::Label* name = new brls::Label();
    name->setText(task.name);
    name->setFontSize(17);
    name->setTextColor(theme["cloud/text"]);
    name->setHeight(22);
    name->setSingleLine(true);
    name->setGrow(1);
    top->addView(name);

    brls::Label* headline = new brls::Label();
    headline->setFontSize(21);
    headline->setHeight(26);
    headline->setMarginLeft(12);
    headline->setTextColor(theme["brls/accent"]);
    headline->setVisibility(brls::Visibility::GONE);
    top->addView(headline);
    card->addView(top);

    ProgressBar* bar = new ProgressBar();
    bar->setHeight(8);
    bar->setMarginTop(10);
    bar->setWidthPercentage(100);
    bar->setTrack(theme["cloud/surface_alt"]);
    bar->setFill(theme["brls/accent"],
                 withAlpha(theme["brls/accent"], 0.55f));
    card->addView(bar);

    brls::Box* meta = new brls::Box(brls::Axis::ROW);
    meta->setAlignItems(brls::AlignItems::CENTER);
    meta->setMarginTop(8);

    if (!task.provider.empty()) {
        brls::Box* badge = new brls::Box(brls::Axis::ROW);
        badge->setAlignItems(brls::AlignItems::CENTER);
        badge->setHeight(20);
        badge->setPadding(0, 8, 0, 8);
        badge->setCornerRadius(10);
        badge->setMarginRight(10);
        badge->setBackgroundColor(theme["cloud/surface_alt"]);

        brls::Label* source = new brls::Label();
        source->setText(task.provider);
        source->setFontSize(11);
        source->setTextColor(theme["cloud/text_secondary"]);
        source->setHeight(15);
        badge->addView(source);
        meta->addView(badge);
    }

    brls::Label* detail = new brls::Label();
    detail->setFontSize(12.5f);
    detail->setTextColor(theme["cloud/text_secondary"]);
    detail->setHeight(18);
    detail->setSingleLine(true);
    detail->setGrow(1);
    detail->setVisibility(brls::Visibility::GONE);
    meta->addView(detail);
    card->addView(meta);

    Row row;
    row.id = task.id;
    row.card = card;
    row.check = check;
    row.name = name;
    row.headline = headline;
    row.detail = detail;
    row.bar = bar;
    row.state = task.state;
    row.lastBytes = task.received;
    row.lastMs = now;
    this->rows.push_back(row);

    return card;
}

brls::Box* DownloadPage::buildFinishedRow(const DownloadTask& task,
                                          uint64_t now) {
    brls::Theme theme = brls::Application::getTheme();

    const std::string hint =
        this->selecting ? "cloud/download/choose"_i18n
                        : "cloud/download/details"_i18n;
    TaskCard* card = new TaskCard(hint, [this, id = task.id] {
        this->runRowAction(id);
    });
    card->setPadding(10, 18, 10, 18);

    brls::Box* top = new brls::Box(brls::Axis::ROW);
    top->setAlignItems(brls::AlignItems::CENTER);

    brls::Label* check = new brls::Label();
    check->setFontSize(15);
    check->setHeight(20);
    check->setMarginRight(8);
    check->setTextColor(theme["brls/accent"]);
    check->setVisibility(this->selecting ? brls::Visibility::VISIBLE
                                         : brls::Visibility::GONE);
    top->addView(check);

    brls::Label* dot = new brls::Label();
    dot->setText("●");
    dot->setFontSize(10);
    dot->setHeight(16);
    dot->setMarginRight(8);
    dot->setTextColor(stateColor(task.state));
    top->addView(dot);

    brls::Label* name = new brls::Label();
    name->setText(task.name);
    name->setFontSize(15);
    name->setTextColor(theme["cloud/text"]);
    name->setHeight(20);
    name->setSingleLine(true);
    name->setGrow(1);
    top->addView(name);

    brls::Label* headline = new brls::Label();
    headline->setFontSize(12.5f);
    headline->setHeight(18);
    headline->setMarginLeft(12);
    headline->setTextColor(stateColor(task.state));
    headline->setVisibility(brls::Visibility::GONE);
    top->addView(headline);

    brls::Label* chevron = new brls::Label();
    chevron->setText("›");
    chevron->setFontSize(18);
    chevron->setHeight(22);
    chevron->setMarginLeft(8);
    chevron->setTextColor(theme["cloud/text_secondary"]);
    top->addView(chevron);
    card->addView(top);

    brls::Box* meta = new brls::Box(brls::Axis::ROW);
    meta->setMarginTop(3);
    meta->setMarginLeft(this->selecting ? 41 : 18);

    brls::Label* detail = new brls::Label();
    detail->setFontSize(12);
    detail->setTextColor(theme["cloud/text_secondary"]);
    detail->setHeight(16);
    detail->setSingleLine(true);
    detail->setGrow(1);
    detail->setVisibility(brls::Visibility::GONE);
    meta->addView(detail);
    card->addView(meta);

    Row row;
    row.id = task.id;
    row.card = card;
    row.check = check;
    row.name = name;
    row.headline = headline;
    row.detail = detail;
    row.bar = nullptr;
    row.state = task.state;
    row.lastBytes = task.received;
    row.lastMs = now;
    this->rows.push_back(row);

    return card;
}

void DownloadPage::updateRow(Row& row, const DownloadTask& task,
                             uint64_t now) {
    brls::Theme theme = brls::Application::getTheme();

    row.name->setText(task.name);

    if (row.check) {
        bool marked = this->isSelected(task.id);
        row.check->setText(marked ? "●" : "○");
        row.check->setTextColor(marked ? theme["brls/accent"]
                                       : theme["cloud/text_secondary"]);
    }

    float progress = 0.0f;
    if (task.total > 0)
        progress = (float)((double)task.received / (double)task.total);
    if (task.state == DownloadState::Done) progress = 1.0f;
    if (auto* bar = dynamic_cast<ProgressBar*>(row.bar))
        bar->setProgress(progress);

    std::string headline;
    std::string detail;
    NVGcolor headlineColor = stateColor(task.state);
    bool detailIsError = false;

    switch (task.state) {
        case DownloadState::Queued:
            headline = stateText(task.state);
            if (task.total > 0) detail = formatBytes(task.total);
            break;

        case DownloadState::Running: {
            // Smooth the instantaneous rate so the number does not jump.
            if (now > row.lastMs && task.received >= row.lastBytes) {
                double instant = (double)(task.received - row.lastBytes) *
                                 1000.0 / (double)(now - row.lastMs);
                row.speed = row.speed <= 0.0
                                ? instant
                                : row.speed * 0.62 + instant * 0.38;
            }
            row.lastBytes = task.received;
            row.lastMs = now;

            if (task.total > 0) {
                int percent = (int)(progress * 100.0f);
                if (percent > 100) percent = 100;
                if (percent < 0) percent = 0;
                headline = std::to_string(percent) + "%";
                detail = formatBytes(task.received) + " / " +
                         formatBytes(task.total);
            } else {
                detail = formatBytes(task.received);
            }

            std::string speedText;
            std::string etaText;
            if (row.speed >= 1.0) {
                speedText = formatBytes((uint64_t)row.speed) + "/s";
                if (task.total > task.received) {
                    uint64_t remaining = task.total - task.received;
                    etaText = "cloud/download/eta"_i18n + " " +
                              formatDuration(
                                  (uint64_t)((double)remaining / row.speed));
                }
            }
            detail = joinMeta({detail, speedText, etaText});
            break;
        }

        case DownloadState::Done:
            headline = stateText(task.state);
            detail = joinMeta({formatBytes(task.total), task.path});
            break;

        case DownloadState::Failed:
            headline = stateText(task.state);
            detail = task.error;
            detailIsError = true;
            break;

        case DownloadState::Cancelled:
            headline = stateText(task.state);
            detail = joinMeta({formatBytes(task.total), task.path});
            break;
    }

    setLabelText(row.headline, headline);
    row.headline->setTextColor(headlineColor);
    setLabelText(row.detail, detail);
    row.detail->setTextColor(detailIsError ? stateColor(DownloadState::Failed)
                                           : theme["cloud/text_secondary"]);
    row.state = task.state;
}

void DownloadPage::updateSummary() {
    // Selection count lives in the slim bar above the list.
    bool selecting = this->selecting;
    if (this->selectBar) {
        this->selectBar->setVisibility(selecting ? brls::Visibility::VISIBLE
                                                 : brls::Visibility::GONE);
        if (selecting && this->selectText)
            this->selectText->setText(
                "cloud/download/selected"_i18n + " " +
                std::to_string(this->selected.size()) + " / " +
                std::to_string(this->latest.size()));
    }

    if (!this->activeSummary) return;

    size_t active = 0;
    uint64_t total = 0;
    uint64_t received = 0;
    for (const auto& task : this->latest) {
        if (!isActive(task.state)) continue;
        active++;
        if (task.total > 0) {
            total += task.total;
            received += std::min(task.received, task.total);
        }
    }

    double speed = 0.0;
    for (const Row& row : this->rows) speed += row.speed;

    std::string summary;
    if (active > 0 && total > 0) {
        int percent = (int)((double)received * 100.0 / (double)total);
        if (percent > 100) percent = 100;
        summary = std::to_string(percent) + "%";
    }
    std::string speedText =
        speed >= 1.0 ? formatBytes((uint64_t)speed) + "/s" : std::string();
    if (!speedText.empty())
        summary = summary.empty() ? speedText : summary + "   " + speedText;

    setLabelText(this->activeSummary, summary);
}

const DownloadTask* DownloadPage::findTask(uint64_t id) const {
    for (const auto& task : this->latest)
        if (task.id == id) return &task;
    return nullptr;
}

bool DownloadPage::isSelected(uint64_t id) const {
    return std::find(this->selected.begin(), this->selected.end(), id) !=
           this->selected.end();
}

void DownloadPage::toggleSelect(uint64_t id) {
    auto it = std::find(this->selected.begin(), this->selected.end(), id);
    if (it != this->selected.end())
        this->selected.erase(it);
    else
        this->selected.push_back(id);
    this->refresh();
}

void DownloadPage::toggleSelectMode() {
    this->selecting = !this->selecting;
    this->selected.clear();
    this->rebuild();
    this->refresh();
}

void DownloadPage::runRowAction(uint64_t id) {
    const DownloadTask* task = this->findTask(id);
    if (!task) return;

    if (this->selecting) {
        this->toggleSelect(id);
        return;
    }
    this->showDetails(id);
}

void DownloadPage::confirmDeleteSelected() {
    std::vector<uint64_t> ids = this->selected;
    if (ids.empty()) return;

    this->showConfirm(
        "cloud/download/delete"_i18n,
        "cloud/download/delete_selected_confirm"_i18n + " (" +
            std::to_string(ids.size()) + ")",
        "cloud/download/delete"_i18n, true, [ids] {
            for (uint64_t id : ids) DownloadManager::instance().remove(id);
        });
}

void DownloadPage::confirmClearFinished() {
    bool anyFinished = false;
    for (const auto& task : this->latest)
        if (!isActive(task.state)) {
            anyFinished = true;
            break;
        }
    if (!anyFinished) return;

    this->showConfirm("cloud/download/clear"_i18n,
                      "cloud/download/clear_confirm"_i18n,
                      "cloud/download/clear"_i18n, false, [] {
                          DownloadManager::instance().clearFinished();
                      });
}

brls::View* DownloadPage::activeTabItem() {
    brls::View* view = this;
    while (view) {
        if (auto* frame = dynamic_cast<AutoTabFrame*>(view)) {
            int index = frame->getActiveIndex();
            return index >= 0 ? frame->getItem(index) : nullptr;
        }
        view = view->getParent();
    }
    return nullptr;
}

void DownloadPage::showConfirm(const std::string& title,
                               const std::string& message,
                               const std::string& confirmText, bool danger,
                               std::function<void()> onConfirm) {
    presentConfirm(title, message, confirmText, danger, std::move(onConfirm));
}

void DownloadPage::showDetails(uint64_t id) {
    const DownloadTask* found = this->findTask(id);
    if (!found) return;
    DownloadTask task = *found;

    brls::Theme theme = brls::Application::getTheme();
    NVGcolor accent = theme["brls/accent"];

    brls::Box* card = makePopupCard(58);

    // Title and state pill.
    brls::Box* header = new brls::Box(brls::Axis::ROW);
    header->setAlignItems(brls::AlignItems::FLEX_START);
    header->setWidthPercentage(100);

    brls::Label* title = new brls::Label();
    title->setText(task.name);
    title->setFontSize(19);
    title->setTextColor(theme["cloud/text"]);
    title->setGrow(1);
    header->addView(title);

    brls::Box* pill = new brls::Box(brls::Axis::ROW);
    pill->setAlignItems(brls::AlignItems::CENTER);
    pill->setHeight(22);
    pill->setPadding(0, 9, 0, 9);
    pill->setMarginLeft(12);
    pill->setCornerRadius(11);
    pill->setBackgroundColor(withAlpha(stateColor(task.state), 0.14f));

    brls::Label* state = new brls::Label();
    state->setText(stateText(task.state));
    state->setFontSize(11.5f);
    state->setHeight(16);
    state->setTextColor(stateColor(task.state));
    pill->addView(state);
    header->addView(pill);
    card->addView(header);

    if (!task.provider.empty()) {
        brls::Label* provider = new brls::Label();
        provider->setText(task.provider);
        provider->setFontSize(12);
        provider->setHeight(18);
        provider->setMarginTop(6);
        provider->setTextColor(theme["cloud/text_secondary"]);
        card->addView(provider);
    }

    // Progress, using the same bar as the task cards.
    if (task.total > 0) {
        float progress = (float)((double)task.received / (double)task.total);
        if (task.state == DownloadState::Done) progress = 1.0f;

        ProgressBar* bar = new ProgressBar();
        bar->setHeight(6);
        bar->setMarginTop(16);
        bar->setWidthPercentage(100);
        bar->setTrack(theme["cloud/surface_alt"]);
        NVGcolor barColor = (task.state == DownloadState::Running ||
                             task.state == DownloadState::Queued)
                                ? accent
                                : stateColor(task.state);
        bar->setFill(barColor, withAlpha(barColor, 0.55f));
        bar->setProgress(progress);
        card->addView(bar);

        brls::Box* sizeRow = new brls::Box(brls::Axis::ROW);
        sizeRow->setAlignItems(brls::AlignItems::CENTER);
        sizeRow->setWidthPercentage(100);
        sizeRow->setMarginTop(8);

        brls::Label* size = new brls::Label();
        size->setText(formatBytes(task.received) + " / " +
                      formatBytes(task.total));
        size->setFontSize(12);
        size->setHeight(18);
        size->setGrow(1);
        size->setTextColor(theme["cloud/text_secondary"]);
        sizeRow->addView(size);

        brls::Label* percent = new brls::Label();
        percent->setText(std::to_string((int)(progress * 100.0f)) + "%");
        percent->setFontSize(12);
        percent->setHeight(18);
        percent->setTextColor(stateColor(task.state));
        sizeRow->addView(percent);
        card->addView(sizeRow);
    }

    if (!task.path.empty())
        addInfoRow(card, "cloud/download/path"_i18n, task.path,
                   theme["cloud/text"]);
    if (!task.error.empty())
        addInfoRow(card, "cloud/download/error"_i18n, task.error,
                   stateColor(DownloadState::Failed));

    // Divider then the actions.
    brls::Box* divider = new brls::Box();
    divider->setHeight(1);
    divider->setWidthPercentage(100);
    divider->setMarginTop(16);
    divider->setBackgroundColor(theme["cloud/border"]);
    card->addView(divider);

    brls::Box* actions = new brls::Box(brls::Axis::ROW);
    actions->setAlignItems(brls::AlignItems::CENTER);
    actions->setJustifyContent(brls::JustifyContent::FLEX_END);
    actions->setWidthPercentage(100);
    actions->setMarginTop(16);

    PillButton* close =
        new PillButton("cloud/download/close"_i18n, false, [] {
            brls::Application::popActivity();
        });
    actions->addView(close);

    PillButton* remove =
        new PillButton("cloud/download/delete_record"_i18n, true, [id] {
            DownloadManager::instance().remove(id);
            brls::Application::popActivity();
        });
    actions->addView(remove);

    PillButton* primary = nullptr;
    if (isActive(task.state)) {
        primary = new PillButton("cloud/download/cancel"_i18n, false, [id] {
            DownloadManager::instance().cancel(id);
            brls::Application::popActivity();
        });
    } else if (task.retryable && (task.state == DownloadState::Failed ||
                                  task.state == DownloadState::Cancelled)) {
        primary = new PillButton("cloud/download/retry"_i18n, false, [id] {
            DownloadManager::instance().retry(id);
            brls::Application::popActivity();
        });
    }

    if (primary) {
        actions->addView(primary);
        primary->dropTrailingMargin();
    } else {
        remove->dropTrailingMargin();
    }

    card->addView(actions);
    presentPopup(card, primary ? (brls::View*)primary : (brls::View*)close);
}

}  // namespace cloud
