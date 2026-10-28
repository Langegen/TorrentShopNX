#pragma once

#include <borealis.hpp>
#include <functional>
#include <string>
#include "dashboard/DashboardIcon.hpp"

namespace ui {

// "Nothing here" block for empty screens: illustrated icon, title, a short
// explanation and an optional action button, centered in the free space.
class EmptyState : public brls::Box {
public:
    EmptyState();

    void show(DashboardIcon::Kind kind, const std::string& title, const std::string& sub,
              const std::string& actionLabel = "", std::function<void()> action = nullptr);

    // What should get focus when the list it replaces disappears: the action
    // button, or the block itself when there is none, so the screen's B and
    // other actions keep working.
    brls::View* actionView();

private:
    brls::Box* iconHolder_ = nullptr;
    brls::Label* title_ = nullptr;
    brls::Label* sub_ = nullptr;
    brls::Button* button_ = nullptr;
    std::function<void()> action_;
};

} // namespace ui
