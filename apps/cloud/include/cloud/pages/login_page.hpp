//
// cloud: QR code login page.
//

#pragma once

#include <borealis.hpp>

#include <memory>
#include <string>

namespace nxkit {
class QRLoginView;
}

namespace cloud {

class LoginPage : public brls::Activity {
public:
    LoginPage();
    ~LoginPage() override;

    brls::View* createContentView() override;

    /// Called by the polling task, public for that reason only.
    void checkQr();

private:
    void requestQr();
    void startPolling();
    void stopPolling();

    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    nxkit::QRLoginView* qrView = nullptr;
    brls::RepeatingTask* pollTask = nullptr;
    std::string qrTokenValue;
    bool finished = false;
};

}  // namespace cloud
