//
// cloud: WebDAV address form shown instead of the QR code login page.
//

#pragma once

#include <memory>
#include <string>

#include <borealis.hpp>

#include "cloud/drive/webdav_session.hpp"

namespace cloud {

/// Enter the collection url and optional account of a WebDAV server. A
/// successful PROPFIND is the equivalent of a successful QR login: the account
/// is saved and the page closes.
class WebDavLoginView : public brls::Box {
public:
    WebDavLoginView();
    ~WebDavLoginView() override;

    void willAppear(bool resetState = false) override;

private:
    enum Field { Url = 0, User, Password, FieldCount };

    void addField(Field field, const std::string& title,
                  const std::string& placeholder);
    void edit(Field field);
    void connect();
    void refreshValues();
    void setStatus(const std::string& text, bool error);

    WebDavConfig form;
    brls::Label* values[FieldCount] = {nullptr};
    brls::View* firstRow = nullptr;
    brls::Label* status = nullptr;
    brls::Button* connectButton = nullptr;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    bool busy = false;
};

}  // namespace cloud
