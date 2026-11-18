//
// cloud: feature tour and about pages.
//

#pragma once

#include <borealis.hpp>

namespace cloud {

/// Full-screen page behind the "功能介绍" / "操作指南" / "关于" tiles of the
/// profile tab. They used to be plain text dialogs; a card layout with drawn
/// glyphs is much easier to read from a couch.
class InfoPage : public brls::Activity {
public:
    enum class Section {
        Features,
        Controls,
        About,
    };

    explicit InfoPage(Section section) : section(section) {}

    brls::View* createContentView() override;

private:
    Section section;
};

}  // namespace cloud
