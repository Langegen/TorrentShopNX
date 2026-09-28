#pragma once

#include <borealis.hpp>
#include <borealis/views/cells/cell_radio.hpp>
#include <string>
#include <vector>
#include <unordered_set>
#include <functional>

namespace ui {

struct MultiSelectItem {
    std::string id;          // Empty string "" represents "All" option
    std::string displayName; // Localized display text
};

class MultiSelectDialog : public brls::Box, private brls::RecyclerDataSource {
public:
    MultiSelectDialog(const std::string& title,
                      const std::vector<MultiSelectItem>& items,
                      const std::vector<std::string>& selectedIds,
                      std::function<void(const std::vector<std::string>&)> onApply);
    ~MultiSelectDialog() override = default;

    static void open(const std::string& title,
                     const std::vector<MultiSelectItem>& items,
                     const std::vector<std::string>& selectedIds,
                     std::function<void(const std::vector<std::string>&)> onApply);

    void show(std::function<void()> cb, bool animate, float animationDuration) override;
    void hide(std::function<void()> cb, bool animated, float animationDuration) override;

protected:
    float getShowAnimationDuration(brls::TransitionAnimation animation) override;
    brls::View* getParentNavigationDecision(brls::View* from, brls::View* newFocus, brls::FocusDirection direction) override;

private:
    int numberOfRows(brls::RecyclerFrame* recycler, int section) override;
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override;
    void didSelectRowAt(brls::RecyclerFrame* recycler, brls::IndexPath index) override;

    void toggleItem(int row);
    void updateVisibleCheckmarks();
    void applyAndDismiss();
    void offsetTick();

    BRLS_BIND(brls::AppletFrame, applet, "brls/multiselect/applet");
    BRLS_BIND(brls::Box, content, "brls/multiselect/content");
    BRLS_BIND(brls::Box, header, "brls/multiselect/header");
    BRLS_BIND(brls::Label, titleLabel, "brls/multiselect/title_label");
    BRLS_BIND(brls::Box, doneBox, "brls/multiselect/done_box");
    BRLS_BIND(brls::Label, doneLabel, "brls/multiselect/done_label");
    BRLS_BIND(brls::RecyclerFrame, recycler, "brls/multiselect/recycler");

    std::vector<MultiSelectItem> items_;
    std::unordered_set<std::string> selectedIds_;
    std::vector<brls::RadioCell*> allocatedCells_;
    std::function<void(const std::vector<std::string>&)> onApply_;
    bool dismissed_ = false;

    brls::Animatable showOffset = 0;
};

} // namespace ui
