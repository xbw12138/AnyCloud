//
// cloud: QR code login page.
//

#include "cloud/pages/login_page.hpp"

#include <borealis/core/task.hpp>

#include <cstdio>

#include "cloud/drive/drive_manager.hpp"
#include "nxkit/ui/qr_login_view.hpp"

using namespace brls::literals;

namespace cloud {

namespace {

const int kPollPeriodMs = 1500;

// Temporary on-device diagnostics, same file the QR view logs to.
void logDebug(const char* line) {
#ifdef __SWITCH__
    FILE* file = fopen("sdmc:/switch/nxkit/qr_debug.log", "a");
#else
    FILE* file = fopen("/tmp/qr_debug.log", "a");
#endif
    if (!file) return;
    fprintf(file, "%s\n", line);
    fclose(file);
}

class QrPollTask : public brls::RepeatingTask {
public:
    explicit QrPollTask(LoginPage* page)
        : brls::RepeatingTask(kPollPeriodMs), page(page) {}

    void run() override { this->page->checkQr(); }

private:
    LoginPage* page;
};

}  // namespace

LoginPage::LoginPage() {}

brls::View* LoginPage::createContentView() {
    brls::Box* box = new brls::Box(brls::Axis::COLUMN);
    box->setGrow(1);
    box->setAlignItems(brls::AlignItems::CENTER);
    box->setJustifyContent(brls::JustifyContent::CENTER);

    brls::Label* title = new brls::Label();
    title->setText("cloud/login/title"_i18n);
    title->setFontSize(28);
    title->setMarginBottom(24);
    box->addView(title);

    this->qrView = new nxkit::QRLoginView();
    this->qrView->setOnRefresh([this] { this->requestQr(); });
    box->addView(this->qrView);

    this->requestQr();
    return box;
}

LoginPage::~LoginPage() {
    *this->alive = false;
    this->stopPolling();
}

void LoginPage::requestQr() {
    this->finished = false;
    this->stopPolling();
    this->qrView->setRefreshVisible(false);
    this->qrView->showLoading("cloud/login/requesting"_i18n);

    auto provider = DriveManager::instance().current();
    provider->qrToken([this, alive = this->alive](const std::string& content,
                                                  const std::string& token,
                                                  const std::string& error) {
        brls::sync([this, alive, content, token, error] {
            if (!*alive) return;
            if (!error.empty() || content.empty()) {
                this->qrView->setStatus(error.empty() ? "cloud/login/failed"_i18n : error);
                this->qrView->setRefreshVisible(true);
                return;
            }
            this->qrTokenValue = token;
            this->qrView->setQrContent(content);
            this->qrView->setStatus("cloud/login/scan"_i18n);
            this->startPolling();
        });
    });
}

void LoginPage::startPolling() {
    this->stopPolling();
    this->pollTask = new QrPollTask(this);
    this->pollTask->start();
}

void LoginPage::stopPolling() {
    if (!this->pollTask) return;
    this->pollTask->stop();
    delete this->pollTask;
    this->pollTask = nullptr;
}

void LoginPage::checkQr() {
    if (this->finished || this->qrTokenValue.empty()) return;

    auto provider = DriveManager::instance().current();
    provider->qrPoll(
        this->qrTokenValue,
        [this, alive = this->alive](QrStatus status, const std::string& error) {
            brls::sync([this, alive, status, error] {
                if (!*alive) return;
                // Transient network errors are retried on the next tick.
                if (!error.empty()) return;

                switch (status) {
                    case QrStatus::Waiting:
                        this->qrView->setStatus("cloud/login/waiting"_i18n);
                        break;
                    case QrStatus::Scanned:
                        this->qrView->setStatus("cloud/login/scanned"_i18n);
                        break;
                    case QrStatus::Expired:
                        this->stopPolling();
                        this->qrView->setStatus("cloud/login/expired"_i18n);
                        this->qrView->setRefreshVisible(true);
                        break;
                    case QrStatus::Success:
                        logDebug("qr: success");
                        this->finished = true;
                        this->stopPolling();
                        this->qrTokenValue.clear();
                        brls::Application::notify("cloud/login/success"_i18n);
                        logDebug("qr: popping");
                        brls::Application::popActivity();
                        break;
                }
            });
        });
}

}  // namespace cloud
