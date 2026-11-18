//
// cloud: main activity with the file browser and profile tabs.
//

#pragma once

#include <borealis.hpp>

namespace cloud {

class MainActivity : public brls::Activity {
public:
    brls::View* createContentView() override;
};

/// Switches the top tab bar to the download manager tab. Used right after a
/// download is queued so the new task is immediately visible.
void focusDownloadsTab();

/// Re-reads the active drive and updates the badge at the right end of the tab
/// bar.
void refreshProviderBadge();

}  // namespace cloud
