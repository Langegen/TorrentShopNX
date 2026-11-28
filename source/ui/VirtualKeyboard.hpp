#pragma once

#include <borealis.hpp>
#include <array>
#include <functional>
#include <string>
#include "../utils/keyboard_layouts.h"

namespace ui {

class VirtualKeyboard : public brls::Box {
public:
    VirtualKeyboard(std::function<void(std::string)> onSubmit,
                    const std::string& headerText,
                    const std::string& subText,
                    int maxStringLength,
                    const std::string& initialText,
                    int kbdDisableBitmask);

    static void open(std::function<void(std::string)> onSubmit,
                     const std::string& headerText = "",
                     const std::string& subText = "",
                     int maxStringLength = 64,
                     const std::string& initialText = "",
                     int kbdDisableBitmask = brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_NONE);

    bool isTranslucent() override { return true; }
    float getShowAnimationDuration(brls::TransitionAnimation animation) override;
    brls::View* getDefaultFocus() override;

private:
    static constexpr int kActionCount = 7;

    bool isShifted() const { return shiftMode_ != util::kbd::ShiftMode::OFF; }
    void setupUi();
    void setupNavigationRoutes();
    void setupShortcuts();

    void onCharKeyPressed(int row, int col);
    void onShiftPressed();
    void onLangTogglePressed();
    void onSymbolsTogglePressed();
    void onCycleLayoutPressed();
    void onSpacePressed();
    void onClearPressed();
    void onBackspacePressed();

    void updateKeyGrid();
    void updateActionButtons();
    void updateDisplay();

    void submitAndDismiss();
    void cancelAndDismiss();

    std::function<void(std::string)> onSubmit_;
    std::string headerText_;
    std::string subText_;
    int maxLen_ = 64;
    int kbdDisableBitmask_ = 0;
    std::string text_;

    util::kbd::Layout layout_ = util::kbd::Layout::EN;
    util::kbd::ShiftMode shiftMode_ = util::kbd::ShiftMode::OFF;
    bool dismissed_ = false;

    static util::kbd::Layout s_lastAlphaLayout;

    brls::Box* panel_ = nullptr;
    brls::Label* titleLabel_ = nullptr;
    brls::Label* modeBadge_ = nullptr;
    brls::Label* counterLabel_ = nullptr;
    brls::Label* inputLabel_ = nullptr;

    std::array<std::array<brls::Box*, util::kbd::kCols>, util::kbd::kRows> charKeys_{};
    std::array<std::array<brls::Label*, util::kbd::kCols>, util::kbd::kRows> charLabels_{};

    std::array<brls::Box*, kActionCount> actionKeys_{};
    std::array<brls::Label*, kActionCount> actionLabels_{};
};

// Unified text input entry point: routes to the built-in keyboard when enabled
// in Settings (or when no platform ImeManager exists), otherwise uses the
// platform IME manager.
bool openTextInput(std::function<void(std::string)> onSubmit,
                   const std::string& headerText = "",
                   const std::string& subText = "",
                   int maxStringLength = 64,
                   const std::string& initialText = "",
                   int kbdDisableBitmask = brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_NONE);

} // namespace ui
