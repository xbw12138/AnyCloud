//
// cloud: WebDAV address form shown instead of the QR code login page.
//

#include "cloud/pages/webdav_login_view.hpp"

#include <cpr/async.h>

#include "cloud/drive/drive_manager.hpp"
#include "cloud/drive/webdav_drive.hpp"

using namespace brls::literals;

namespace cloud {

namespace {

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string trimmed(const std::string& text) {
    size_t begin = 0;
    while (begin < text.size() && isSpace(text[begin])) begin++;
    size_t end = text.size();
    while (end > begin && isSpace(text[end - 1])) end--;
    return text.substr(begin, end - begin);
}

/// One editable form row: title on the left, current value on the right.
class FieldRow : public brls::Box {
public:
    FieldRow(const std::string& title, brls::Label* value,
             std::function<void()> onEdit)
        : onEdit(std::move(onEdit)) {
        brls::Theme theme = brls::Application::getTheme();

        this->setAxis(brls::Axis::ROW);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setHeight(64);
        this->setPadding(0, 22, 0, 22);
        this->setCornerRadius(16);
        this->setHighlightCornerRadius(16);
        this->setBorderColor(theme["brls/accent"]);
        this->setBorderThickness(0);
        this->setMarginBottom(12);
        this->setFocusable(true);

        brls::Label* name = new brls::Label();
        name->setText(title);
        name->setFontSize(18);
        name->setWidth(180);
        name->setTextColor(theme["cloud/text_secondary"]);
        this->addView(name);
        this->addView(value);

        this->registerAction("cloud/webdav/edit"_i18n, brls::BUTTON_A,
                             [this](brls::View*) {
                                 if (this->onEdit) this->onEdit();
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

private:
    void refreshStyle() {
        brls::Theme theme = brls::Application::getTheme();
        bool focused = this->isFocused();
        this->setBackgroundColor(focused ? theme["cloud/surface_alt"]
                                         : theme["cloud/surface"]);
        this->setBorderThickness(focused ? 2.0f : 0.0f);
    }

    std::function<void()> onEdit;
};

}  // namespace

WebDavLoginView::WebDavLoginView() {
    brls::Theme theme = brls::Application::getTheme();

    this->setAxis(brls::Axis::COLUMN);
    this->setWidth(820);
    this->setAlignItems(brls::AlignItems::STRETCH);

    brls::Label* title = new brls::Label();
    title->setText("cloud/webdav/title"_i18n);
    title->setFontSize(28);
    title->setMarginBottom(20);
    title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    this->addView(title);

    // Reuse the stored account when the user opens the form again.
    auto provider = std::dynamic_pointer_cast<WebDavDrive>(
        DriveManager::instance().find("webdav"));
    if (!provider)
        provider = std::dynamic_pointer_cast<WebDavDrive>(
            DriveManager::instance().current());
    if (provider) this->form = provider->config();

    this->addField(Url, "cloud/webdav/url"_i18n, "cloud/webdav/url_value"_i18n);
    this->addField(User, "cloud/webdav/user"_i18n,
                   "cloud/webdav/user_value"_i18n);
    this->addField(Password, "cloud/webdav/password"_i18n,
                   "cloud/webdav/password_value"_i18n);

    brls::Label* hint = new brls::Label();
    hint->setText("cloud/webdav/hint"_i18n);
    hint->setFontSize(14);
    hint->setMarginTop(4);
    hint->setTextColor(theme["cloud/text_secondary"]);
    hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    this->addView(hint);

    this->connectButton = new brls::Button();
    this->connectButton->setText("cloud/webdav/connect"_i18n);
    this->connectButton->setMarginTop(16);
    this->connectButton->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    this->connectButton->registerClickAction([this](brls::View*) {
        this->connect();
        return true;
    });
    this->addView(this->connectButton);

    this->status = new brls::Label();
    this->status->setFontSize(15);
    this->status->setMarginTop(14);
    this->status->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    this->status->setVisibility(brls::Visibility::GONE);
    this->addView(this->status);

    // X connects from anywhere in the form, matching the hint.
    this->registerAction("cloud/webdav/connect"_i18n, brls::BUTTON_X,
                         [this](brls::View*) {
                             this->connect();
                             return true;
                         });
    this->registerAction("hints/back"_i18n, brls::BUTTON_B,
                         [](brls::View*) {
                             brls::Application::popActivity();
                             return true;
                         });

    this->refreshValues();
}

void WebDavLoginView::willAppear(bool resetState) {
    brls::Box::willAppear(resetState);
    if (this->firstRow) brls::Application::giveFocus(this->firstRow);
}

WebDavLoginView::~WebDavLoginView() { *this->alive = false; }

void WebDavLoginView::addField(Field field, const std::string& title,
                               const std::string& placeholder) {
    brls::Theme theme = brls::Application::getTheme();

    brls::Label* value = new brls::Label();
    value->setFontSize(18);
    value->setGrow(1);
    value->setHeight(24);
    value->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    value->setTextColor(theme["cloud/text"]);
    value->setText(placeholder);
    this->values[field] = value;

    this->addView(new FieldRow(title, value, [this, field] {
        this->edit(field);
    }));

    if (!this->firstRow) this->firstRow = this->getChildren().back();
}

void WebDavLoginView::refreshValues() {
    brls::Theme theme = brls::Application::getTheme();
    auto show = [&](Field field, const std::string& text,
                    const std::string& placeholder) {
        brls::Label* label = this->values[field];
        if (!label) return;
        label->setText(text.empty() ? placeholder : text);
        label->setTextColor(text.empty() ? theme["cloud/text_secondary"]
                                         : theme["cloud/text"]);
    };

    show(Url, this->form.url, "cloud/webdav/url_value"_i18n);
    show(User, this->form.user, "cloud/webdav/user_value"_i18n);
    show(Password, this->form.password.empty() ? "" : "••••••",
         "cloud/webdav/password_value"_i18n);
}

void WebDavLoginView::edit(Field field) {
    std::string title;
    std::string subheader;
    std::string initial;
    int maxLength = 128;

    switch (field) {
        case Url:
            title = "cloud/webdav/url"_i18n;
            subheader = "cloud/webdav/url_value"_i18n;
            // The keyboard is narrow; a full collection url is long.
            maxLength = 200;
            initial = this->form.url;
            break;
        case User:
            title = "cloud/webdav/user"_i18n;
            subheader = "cloud/webdav/user_value"_i18n;
            initial = this->form.user;
            break;
        case Password:
            title = "cloud/webdav/password"_i18n;
            // Hand the stored password back so it never has to be retyped.
            initial = this->form.password;
            break;
        default:
            return;
    }

    brls::Application::getImeManager()->openForText(
        [this, field](std::string text) {
            std::string value = trimmed(text);
            switch (field) {
                case Url:
                    this->form.url = value;
                    break;
                case User:
                    this->form.user = value;
                    break;
                case Password:
                    // An empty reply is a cancelled keyboard, keep the old one.
                    if (!value.empty()) this->form.password = text;
                    break;
                default:
                    break;
            }
            this->refreshValues();
        },
        title, subheader, maxLength, initial);
}

void WebDavLoginView::setStatus(const std::string& text, bool error) {
    if (!this->status) return;
    this->status->setText(text);
    this->status->setTextColor(error ? nvgRGB(226, 86, 86)
                                     : brls::Application::getTheme()
                                           ["cloud/text_secondary"]);
    this->status->setVisibility(text.empty() ? brls::Visibility::GONE
                                             : brls::Visibility::VISIBLE);
}

void WebDavLoginView::connect() {
    if (this->busy) return;

    WebDavConfig config = this->form;
    config.url = webDavNormalizeUrl(config.url);
    config.user = trimmed(config.user);

    if (!config.valid()) {
        this->setStatus("cloud/webdav/required"_i18n, true);
        return;
    }

    auto provider = std::dynamic_pointer_cast<WebDavDrive>(
        DriveManager::instance().current());
    if (!provider) {
        this->setStatus("cloud/webdav/failed"_i18n, true);
        return;
    }

    this->busy = true;
    this->setStatus("cloud/webdav/connecting"_i18n, false);
    if (this->connectButton)
        this->connectButton->setState(brls::ButtonState::DISABLED);

    cpr::async([this, alive = this->alive, config, provider] {
        WebDavSession session(config);
        std::string error;
        const bool ok = session.probe(error);
        const long status = session.lastStatus();
        brls::sync([this, alive, config, provider, ok, error, status] {
            if (!*alive) return;
            this->busy = false;
            if (this->connectButton)
                this->connectButton->setState(brls::ButtonState::ENABLED);
            if (!ok) {
                if (status == 401 || status == 403)
                    this->setStatus("cloud/webdav/auth_failed"_i18n, true);
                else
                    this->setStatus(error.empty() ? "cloud/webdav/failed"_i18n
                                                  : error,
                                    true);
                return;
            }
            provider->applyConfig(config);
            // The pages behind this form were built while the account was
            // still unset, so re-activate the drive: that bumps the manager's
            // generation and makes the browser reload at the collection root.
            DriveManager::instance().setCurrent(provider->id());
            brls::Application::notify("cloud/webdav/success"_i18n);
            brls::Application::popActivity();
        });
    });
}

}  // namespace cloud
