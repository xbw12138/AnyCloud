//
// cloud: in-app network log viewer.
//

#pragma once

#include <borealis.hpp>

#include "nxkit/http_log.hpp"

namespace cloud {

/// Shows every HTTP call made by the app (plus cover cache decisions) so a
/// failing provider can be diagnosed without a PC. Newest entry first.
class HttpLogPage : public brls::Activity {
public:
    brls::View* createContentView() override;

private:
    class LogView;

    void rebuild();
    void showDetail(const nxkit::HttpLogEntry& entry);

    LogView* list = nullptr;
    brls::Label* summary = nullptr;
};

}  // namespace cloud
