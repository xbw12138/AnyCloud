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

}  // namespace cloud
