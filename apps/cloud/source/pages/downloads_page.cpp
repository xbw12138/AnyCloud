//
// cloud: download task manager page.
//

#include "cloud/pages/downloads_page.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>

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
            return nvgRGB(88, 199, 128);
        case DownloadState::Failed:
            return nvgRGB(232, 104, 104);
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

/// Thin rounded progress track with an accent fill.
class ThinBar : public brls::View {
public:
    void setProgress(float value) {
        float next = std::min(1.0f, std::max(0.0f, value));
        if (next == this->progress) return;
        this->progress = next;
        this->invalidate();
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (width <= 0 || height <= 0) return;
        brls::Theme theme = brls::Application::getTheme();
        float radius = height / 2.0f;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, radius);
        nvgFillColor(vg, this->a(theme["cloud/border"]));
        nvgFill(vg);

        if (this->progress <= 0) return;
        float fillWidth = std::max(height, width * this->progress);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, fillWidth, height, radius);
        nvgFillColor(vg, this->a(theme["brls/accent"]));
        nvgFill(vg);
    }

private:
    float progress = 0;
};

/// One download card. Highlight is drawn manually because a plain Box does
/// not show focus on its own.
class TaskCard : public brls::Box {
public:
    TaskCard(const std::string& hint, std::function<void()> activate)
        : activate(std::move(activate)) {
        this->setAxis(brls::Axis::COLUMN);
        this->setPadding(14, 18, 14, 18);
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
    *this->alive = false;
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

    brls::Box* header = new brls::Box(brls::Axis::ROW);
    header->setAlignItems(brls::AlignItems::CENTER);
    header->setJustifyContent(brls::JustifyContent::FLEX_END);
    header->setMarginTop(4);
    header->setMarginBottom(12);

    brls::Box* pill = new brls::Box(brls::Axis::ROW);
    pill->setAlignItems(brls::AlignItems::CENTER);
    pill->setHeight(30);
    pill->setPadding(0, 14, 0, 14);
    pill->setCornerRadius(15);
    pill->setBackgroundColor(theme["cloud/surface_alt"]);

    this->summaryLabel = new brls::Label();
    this->summaryLabel->setFontSize(13);
    this->summaryLabel->setTextColor(theme["cloud/text_secondary"]);
    this->summaryLabel->setHeight(17);
    pill->addView(this->summaryLabel);
    header->addView(pill);
    this->addView(header);

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

void DownloadPage::refresh() {
    std::vector<DownloadTask> tasks = DownloadManager::instance().snapshot();

    bool stale = tasks.size() != this->rows.size();
    if (!stale) {
        for (size_t i = 0; i < tasks.size(); i++) {
            if (tasks[i].id != this->rows[i].id ||
                tasks[i].state != this->rows[i].state) {
                stale = true;
                break;
            }
        }
    }

    this->latest = tasks;
    if (stale) this->rebuild();

    uint64_t now = nowMs();
    size_t count = std::min(this->latest.size(), this->rows.size());
    for (size_t i = 0; i < count; i++)
        this->updateRow(this->rows[i], this->latest[i], now);
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

void DownloadPage::rebuild() {
    // Keep the highlight on the same task after a rebuild.
    uint64_t focusedId = 0;
    for (const Row& row : this->rows)
        if (row.card && row.card->isFocused()) focusedId = row.id;

    brls::Theme theme = brls::Application::getTheme();
    this->listBox->clearViews();
    this->rows.clear();
    uint64_t now = nowMs();

    // Forget selections of tasks that no longer exist.
    this->selected.erase(
        std::remove_if(this->selected.begin(), this->selected.end(),
                       [this](uint64_t id) { return !this->findTask(id); }),
        this->selected.end());
    if (this->latest.empty()) this->selecting = false;

    if (this->latest.empty()) {
        brls::Box* empty = new brls::Box(brls::Axis::COLUMN);
        empty->setGrow(1);
        empty->setAlignItems(brls::AlignItems::CENTER);
        empty->setJustifyContent(brls::JustifyContent::CENTER);

        brls::Label* title = new brls::Label();
        title->setText("cloud/download/empty"_i18n);
        title->setFontSize(20);
        title->setTextColor(theme["cloud/text_secondary"]);
        title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        title->setMarginBottom(8);
        empty->addView(title);

        brls::Label* hint = new brls::Label();
        hint->setText("cloud/download/empty_hint"_i18n);
        hint->setFontSize(14);
        hint->setTextColor(theme["cloud/text_secondary"]);
        hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        empty->addView(hint);
        this->listBox->addView(empty);
        return;
    }

    Row* focusedRow = nullptr;

    for (const auto& task : this->latest) {
        uint64_t id = task.id;
        bool active = isActive(task.state);
        const std::string hint =
            this->selecting ? "cloud/download/choose"_i18n
                            : (active ? "cloud/download/cancel"_i18n
                                      : "cloud/download/details"_i18n);
        TaskCard* card = new TaskCard(hint, [this, id] {
            this->runRowAction(id);
        });

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

        brls::Label* status = new brls::Label();
        status->setFontSize(13);
        status->setHeight(18);
        status->setMarginLeft(12);
        top->addView(status);
        card->addView(top);

        ThinBar* bar = new ThinBar();
        bar->setHeight(6);
        bar->setMarginTop(10);
        bar->setWidthPercentage(100);
        card->addView(bar);

        // Source tag + progress/error text.
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
        detail->setFontSize(13);
        detail->setTextColor(theme["cloud/text_secondary"]);
        detail->setHeight(18);
        detail->setSingleLine(true);
        detail->setGrow(1);
        meta->addView(detail);
        card->addView(meta);

        this->listBox->addView(card);

        Row row;
        row.id = task.id;
        row.card = card;
        row.check = check;
        row.name = name;
        row.status = status;
        row.detail = detail;
        row.bar = bar;
        row.state = task.state;
        row.lastBytes = task.received;
        row.lastMs = now;
        this->rows.push_back(row);
    }

    for (Row& row : this->rows)
        if (row.id == focusedId) focusedRow = &row;

    if (focusedRow)
        brls::Application::giveFocus(focusedRow->card);
    else if (!this->rows.empty())
        brls::Application::giveFocus(this->rows.front().card);
}

void DownloadPage::updateRow(Row& row, const DownloadTask& task,
                             uint64_t now) {
    brls::Theme theme = brls::Application::getTheme();

    row.name->setText(task.name);
    row.status->setText(stateText(task.state));
    row.status->setTextColor(stateColor(task.state));

    if (row.check) {
        bool marked = this->isSelected(task.id);
        row.check->setText(marked ? "●" : "○");
        row.check->setTextColor(marked ? theme["brls/accent"]
                                       : theme["cloud/text_secondary"]);
    }

    ThinBar* bar = dynamic_cast<ThinBar*>(row.bar);
    float progress = 0.0f;
    if (task.total > 0)
        progress = (float)((double)task.received / (double)task.total);
    if (task.state == DownloadState::Done) progress = 1.0f;
    if (bar) {
        bar->setProgress(progress);
        bar->setVisibility((task.state == DownloadState::Running ||
                            task.state == DownloadState::Done)
                               ? brls::Visibility::VISIBLE
                               : brls::Visibility::GONE);
    }

    std::string detail;
    switch (task.state) {
        case DownloadState::Queued:
            detail = "cloud/download/queued"_i18n;
            break;
        case DownloadState::Running: {
            if (task.total > 0) {
                int percent = (int)(progress * 100.0f);
                if (percent > 100) percent = 100;
                detail = std::to_string(percent) + "%  ·  " +
                         formatBytes(task.received) + " / " +
                         formatBytes(task.total);
                if (now > row.lastMs && task.received >= row.lastBytes) {
                    uint64_t speed =
                        (task.received - row.lastBytes) * 1000 /
                        (now - row.lastMs);
                    if (speed > 0) detail += "  ·  " + formatBytes(speed) + "/s";
                }
            } else {
                detail = formatBytes(task.received);
            }
            row.lastBytes = task.received;
            row.lastMs = now;
            break;
        }
        case DownloadState::Done:
            detail = task.path;
            break;
        case DownloadState::Failed:
            detail = task.error;
            break;
        case DownloadState::Cancelled:
            detail = task.path;
            break;
    }
    row.detail->setText(detail);
    row.detail->setTextColor(task.state == DownloadState::Failed
                                 ? stateColor(task.state)
                                 : theme["cloud/text_secondary"]);
    row.state = task.state;
}

void DownloadPage::updateSummary() {
    if (this->selecting) {
        std::string text = "cloud/download/selected"_i18n + " " +
                           std::to_string(this->selected.size()) + " / " +
                           std::to_string(this->latest.size());
        this->summaryLabel->setText(text);
        return;
    }

    size_t active = 0;
    for (const auto& task : this->latest)
        if (isActive(task.state)) active++;

    std::string text = std::to_string(this->latest.size()) + " " +
                       "cloud/download/tasks"_i18n;
    if (active > 0)
        text += "  ·  " + std::to_string(active) + " " +
                "cloud/download/running"_i18n;
    this->summaryLabel->setText(text);
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
    if (isActive(task->state))
        this->confirmCancel(id);
    else
        this->showDetails(id);
}

void DownloadPage::confirmCancel(uint64_t id) {
    brls::Dialog* dialog =
        new brls::Dialog("cloud/download/cancel_confirm"_i18n);
    dialog->addButton("cloud/download/keep"_i18n, [] {});
    dialog->addButton("cloud/download/cancel"_i18n, [id] {
        DownloadManager::instance().cancel(id);
    });
    dialog->setCancelable(true);
    dialog->open();
}

void DownloadPage::confirmDeleteSelected() {
    std::vector<uint64_t> ids = this->selected;
    if (ids.empty()) return;

    brls::Dialog* dialog = new brls::Dialog(
        "cloud/download/delete_selected_confirm"_i18n + " (" +
        std::to_string(ids.size()) + ")");
    dialog->addButton("cloud/files/action/cancel"_i18n, [] {});
    dialog->addButton("cloud/download/delete"_i18n, [ids] {
        for (uint64_t id : ids) DownloadManager::instance().remove(id);
    });
    dialog->setCancelable(true);
    dialog->open();
}

void DownloadPage::confirmClearFinished() {
    bool anyFinished = false;
    for (const auto& task : this->latest)
        if (!isActive(task.state)) {
            anyFinished = true;
            break;
        }
    if (!anyFinished) return;

    brls::Dialog* dialog =
        new brls::Dialog("cloud/download/clear_confirm"_i18n);
    dialog->addButton("cloud/files/action/cancel"_i18n, [] {});
    dialog->addButton("cloud/download/clear"_i18n, [] {
        DownloadManager::instance().clearFinished();
    });
    dialog->setCancelable(true);
    dialog->open();
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

void DownloadPage::showDetails(uint64_t id) {
    const DownloadTask* found = this->findTask(id);
    if (!found) return;
    DownloadTask task = *found;

    std::string message = task.name + "\n\n";
    message += "cloud/download/source"_i18n + ": " +
               (task.provider.empty() ? std::string("-") : task.provider) +
               "\n";
    message += "cloud/download/status"_i18n + ": " + stateText(task.state) +
               "\n";
    if (!task.path.empty())
        message += "cloud/download/path"_i18n + ": " + task.path + "\n";
    if (task.total > 0)
        message += "cloud/download/size"_i18n + ": " +
                   formatBytes(task.received) + " / " +
                   formatBytes(task.total) + "\n";
    if (!task.error.empty())
        message += "cloud/download/error"_i18n + ": " + task.error;

    brls::Dialog* dialog = new brls::Dialog(message);
    if (task.retryable && (task.state == DownloadState::Failed ||
                           task.state == DownloadState::Cancelled)) {
        dialog->addButton("cloud/download/retry"_i18n, [id] {
            DownloadManager::instance().retry(id);
        });
    }
    dialog->addButton("cloud/download/delete_record"_i18n, [id] {
        DownloadManager::instance().remove(id);
    });
    dialog->addButton("cloud/download/close"_i18n, [] {});
    dialog->setCancelable(true);
    dialog->open();
}

}  // namespace cloud
