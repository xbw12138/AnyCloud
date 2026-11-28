//
// cloud: SMB share form shown instead of the QR code login page.
//

#include "cloud/pages/smb_login_view.hpp"

#include <cpr/async.h>

#include "cloud/drive/drive_manager.hpp"
#include "cloud/drive/smb_bridge.hpp"
#include "cloud/drive/smb_drive.hpp"

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
        name->setWidth(220);
        name->setTextColor(theme["cloud/text_secondary"]);
        this->addView(name);
        this->addView(value);

        this->registerAction("cloud/smb/edit"_i18n, brls::BUTTON_A,
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

SmbLoginView::SmbLoginView() {
    brls::Theme theme = brls::Application::getTheme();

    this->setAxis(brls::Axis::COLUMN);
    this->setWidth(760);
    this->setAlignItems(brls::AlignItems::STRETCH);

    brls::Label* title = new brls::Label();
    title->setText("cloud/smb/title"_i18n);
    title->setFontSize(28);
    title->setMarginBottom(20);
    title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    this->addView(title);

    // Reuse the stored share when the user opens the form again.
    auto provider = std::dynamic_pointer_cast<SmbDrive>(
        DriveManager::instance().find("smb"));
    if (!provider)
        provider = std::dynamic_pointer_cast<SmbDrive>(
            DriveManager::instance().current());
    if (provider) this->form = provider->config();

    this->addField(Server, "cloud/smb/server"_i18n, "cloud/smb/server_value"_i18n);
    this->addField(Share, "cloud/smb/share"_i18n, "cloud/smb/share_value"_i18n);
    this->addField(User, "cloud/smb/user"_i18n, "cloud/smb/user_value"_i18n);
    this->addField(Password, "cloud/smb/password"_i18n,
                   "cloud/smb/password_value"_i18n);

    brls::Label* hint = new brls::Label();
    hint->setText("cloud/smb/hint"_i18n);
    hint->setFontSize(14);
    hint->setMarginTop(4);
    hint->setTextColor(theme["cloud/text_secondary"]);
    hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    this->addView(hint);

    this->connectButton = new brls::Button();
    this->connectButton->setText("cloud/smb/connect"_i18n);
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
    this->registerAction("cloud/smb/connect"_i18n, brls::BUTTON_X,
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

void SmbLoginView::willAppear(bool resetState) {
    brls::Box::willAppear(resetState);
    if (this->firstRow) brls::Application::giveFocus(this->firstRow);
}

SmbLoginView::~SmbLoginView() { *this->alive = false; }

void SmbLoginView::addField(Field field, const std::string& title,
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

void SmbLoginView::refreshValues() {
    brls::Theme theme = brls::Application::getTheme();
    auto show = [&](Field field, const std::string& text,
                    const std::string& placeholder, bool placeholderStyle) {
        brls::Label* label = this->values[field];
        if (!label) return;
        label->setText(text.empty() ? placeholder : text);
        label->setTextColor(placeholderStyle || text.empty()
                                ? theme["cloud/text_secondary"]
                                : theme["cloud/text"]);
    };

    show(Server, this->form.server, "cloud/smb/server_value"_i18n, false);
    show(Share, this->form.share, "cloud/smb/share_value"_i18n, false);
    show(User, this->form.user, "cloud/smb/user_value"_i18n, false);
    show(Password, this->form.password.empty() ? "" : "••••••",
         "cloud/smb/password_value"_i18n, false);
}

void SmbLoginView::edit(Field field) {
    std::string title;
    std::string subheader;
    std::string initial;
    int maxLength = 128;

    switch (field) {
        case Server:
            title = "cloud/smb/server"_i18n;
            subheader = "cloud/smb/server_value"_i18n;
            initial = this->form.server;
            break;
        case Share:
            title = "cloud/smb/share"_i18n;
            subheader = "cloud/smb/share_value"_i18n;
            initial = this->form.share;
            break;
        case User:
            title = "cloud/smb/user"_i18n;
            subheader = "cloud/smb/user_value"_i18n;
            initial = this->form.user;
            break;
        case Password:
            title = "cloud/smb/password"_i18n;
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
                case Server:
                    this->form.server = smbNormalizeServer(value);
                    break;
                case Share:
                    this->form.share = value;
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

void SmbLoginView::setStatus(const std::string& text, bool error) {
    if (!this->status) return;
    this->status->setText(text);
    this->status->setTextColor(error ? nvgRGB(226, 86, 86)
                                     : brls::Application::getTheme()
                                           ["cloud/text_secondary"]);
    this->status->setVisibility(text.empty() ? brls::Visibility::GONE
                                             : brls::Visibility::VISIBLE);
}

void SmbLoginView::connect() {
    if (this->busy) return;

    SmbConfig config = this->form;
    config.server = smbNormalizeServer(config.server);
    config.share = trimmed(config.share);
    if (config.share.size() > 1 && config.share[0] == '/')
        config.share.erase(config.share.begin());
    while (!config.share.empty() && config.share.back() == '/')
        config.share.pop_back();

    if (!config.valid()) {
        this->setStatus("cloud/smb/required"_i18n, true);
        return;
    }

    auto provider = std::dynamic_pointer_cast<SmbDrive>(
        DriveManager::instance().current());
    if (!provider) {
        this->setStatus("cloud/smb/failed"_i18n, true);
        return;
    }

    this->busy = true;
    this->setStatus("cloud/smb/connecting"_i18n, false);
    if (this->connectButton)
        this->connectButton->setState(brls::ButtonState::DISABLED);

    cpr::async([this, alive = this->alive, config, provider] {
        SmbSession session(config);
        std::string error;
        bool ok = session.connect(error);
        if (ok) {
            // Connecting is not enough, the share root must be listable.
            std::vector<SmbEntry> entries;
            ok = session.list("/", entries, error);
        }
        brls::sync([this, alive, config, provider, ok, error] {
            if (!*alive) return;
            this->busy = false;
            if (this->connectButton)
                this->connectButton->setState(brls::ButtonState::ENABLED);
            if (!ok) {
                this->setStatus(error.empty() ? "cloud/smb/failed"_i18n : error,
                                true);
                return;
            }
            provider->applyConfig(config);
            // Bring the local http bridge up now, while the form is still on
            // screen: the first media open then only builds a url instead of
            // starting a server in the middle of the player's setup.
            if (SmbBridge::instance().start().empty()) {
                this->setStatus("cloud/smb/failed"_i18n, true);
                return;
            }
            // The pages behind this form were built while the share was still
            // unset, so re-activate the drive: that bumps the manager's
            // generation and makes the browser reload at the share root.
            DriveManager::instance().setCurrent(provider->id());
            brls::Application::notify("cloud/smb/success"_i18n);
            brls::Application::popActivity();
        });
    });
}

}  // namespace cloud
