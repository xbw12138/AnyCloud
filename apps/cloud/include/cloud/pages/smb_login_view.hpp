//
// cloud: SMB share form shown instead of the QR code login page.
//

#pragma once

#include <memory>
#include <string>

#include <borealis.hpp>

#include "cloud/drive/smb_session.hpp"

namespace cloud {

/// Enter the server / share / credentials of an SMB share. A successful test
/// connection is the equivalent of a successful QR login: the share is saved
/// and the page closes.
class SmbLoginView : public brls::Box {
public:
    SmbLoginView();
    ~SmbLoginView() override;

    void willAppear(bool resetState = false) override;

private:
    enum Field { Server = 0, Share, User, Password, FieldCount };

    void addField(Field field, const std::string& title,
                  const std::string& placeholder);
    void edit(Field field);
    void connect();
    void refreshValues();
    void setStatus(const std::string& text, bool error);

    SmbConfig form;
    brls::Label* values[FieldCount] = {nullptr};
    brls::View* firstRow = nullptr;
    brls::Label* status = nullptr;
    brls::Button* connectButton = nullptr;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    bool busy = false;
};

}  // namespace cloud
