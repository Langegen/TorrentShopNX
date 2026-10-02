#include "MultiSelectDialog.hpp"
#include <borealis/views/cells/cell_radio.hpp>
#include <algorithm>

namespace ui {

static const std::string multiSelectFrameXML = R"xml(
<brls:Box
    width="1280"
    height="auto"
    axis="column">

    <brls:AppletFrame
        id="brls/multiselect/applet"
        width="auto"
        height="auto"
        grow="1"
        headerHidden="true"
        footerHidden="true"
        backgroundColor="@theme/brls/backdrop">

        <brls:Box
            width="auto"
            height="auto"
            grow="1"
            axis="column"
            justifyContent="flexEnd">

            <brls:Box
                id="brls/multiselect/content"
                width="auto"
                height="330"
                axis="column"
                backgroundColor="@theme/brls/background">

                <brls:Box
                    id="brls/multiselect/header"
                    width="auto"
                    height="@style/brls/dropdown/header_height"
                    axis="row"
                    alignItems="center"
                    paddingTop="@style/brls/applet_frame/header_padding_top_bottom"
                    paddingBottom="@style/brls/applet_frame/header_padding_top_bottom"
                    paddingLeft="@style/brls/applet_frame/header_padding_sides"
                    paddingRight="@style/brls/applet_frame/header_padding_sides"
                    marginLeft="@style/brls/applet_frame/padding_sides"
                    marginRight="@style/brls/applet_frame/padding_sides"
                    lineColor="@theme/brls/applet_frame/separator"
                    lineBottom="1px">

                    <brls:Label
                        id="brls/multiselect/title_label"
                        width="auto"
                        height="auto"
                        grow="1"
                        marginTop="@style/brls/applet_frame/header_title_top_offset"
                        fontSize="@style/brls/dropdown/header_title_font_size" />

                    <brls:Box
                        id="brls/multiselect/done_box"
                        width="auto"
                        height="auto"
                        paddingLeft="24"
                        paddingRight="8"
                        paddingTop="6"
                        paddingBottom="6">

                        <brls:Label
                            id="brls/multiselect/done_label"
                            width="auto"
                            height="auto"
                            fontSize="@style/brls/dropdown/header_title_font_size"
                            textColor="@theme/brls/list/listItem_value_color" />

                    </brls:Box>

                </brls:Box>
            
                <brls:Box
                    width="auto"
                    height="auto"
                    axis="row"
                    grow="1"
                    justifyContent="center"
                    alignItems="stretch">
                    
                    <brls:RecyclerFrame
                        id="brls/multiselect/recycler"
                        width="100%"
                        height="auto"
                        paddingTop="@style/brls/dropdown/listPadding"
                        paddingRight="@style/brls/dropdown/listPaddingSides"
                        paddingBottom="@style/brls/dropdown/listPadding"
                        paddingLeft="@style/brls/dropdown/listPaddingSides"/>

                </brls:Box>

            </brls:Box>

        </brls:Box>

    </brls:AppletFrame>
    <brls:BottomBar
        backgroundColor="@theme/brls/background"/>
</brls:Box>
)xml";

MultiSelectDialog::MultiSelectDialog(const std::string& title,
                                     const std::vector<MultiSelectItem>& items,
                                     const std::vector<std::string>& selectedIds,
                                     std::function<void(const std::vector<std::string>&)> onApply)
    : items_(items), onApply_(std::move(onApply)) {
    this->inflateFromXMLString(multiSelectFrameXML);
    this->titleLabel->setText(title);

    for (const auto& s : selectedIds) {
        if (!s.empty()) {
            selectedIds_.insert(s);
        }
    }

    doneLabel->setText("app/filter/apply_done"_i18n);
    doneBox->addGestureRecognizer(new brls::TapGestureRecognizer([this](brls::TapGestureStatus status, brls::Sound* soundToPlay) {
        if (status.state == brls::GestureState::END) {
            this->applyAndDismiss();
        }
    }));

    recycler->estimatedRowHeight = brls::Application::getStyle()["brls/dropdown/listItemHeight"];
    recycler->registerCell("Cell", [this]() {
        brls::RadioCell* cell = new brls::RadioCell();
        cell->setHeight(brls::Application::getStyle()["brls/dropdown/listItemHeight"]);
        cell->title->setFontSize(brls::Application::getStyle()["brls/dropdown/listItemTextSize"]);

        cell->updateActionHint(brls::BUTTON_A, "app/filter/hint_select_one"_i18n);

        cell->registerAction("app/filter/hint_multiselect"_i18n, brls::BUTTON_Y, [this, cell](brls::View* view) {
            this->toggleItem(cell->getIndexPath().row);
            return true;
        }, false, false, brls::SOUND_CLICK);

        cell->registerAction("app/filter/hint_apply"_i18n, brls::BUTTON_B, [this](brls::View* view) {
            this->applyAndDismiss();
            return true;
        }, false, false, brls::SOUND_BACK);

        allocatedCells_.push_back(cell);
        return cell;
    });

    int initialFocusRow = 0;
    if (!selectedIds_.empty()) {
        for (size_t i = 1; i < items_.size(); ++i) {
            if (selectedIds_.count(items_[i].id)) {
                initialFocusRow = static_cast<int>(i);
                break;
            }
        }
    }
    recycler->setDefaultCellFocus(brls::IndexPath(0, initialFocusRow));
    recycler->setDataSource(this, false);

    brls::Style style = brls::Application::getStyle();
    float height = numberOfRows(recycler, 0) * style["brls/dropdown/listItemHeight"]
        + header->getHeight()
        + style["brls/dropdown/listPadding"]
        + style["brls/dropdown/listPadding"];

    content->setHeight(std::min(height, brls::Application::contentHeight * 0.73f));

    this->registerAction("app/filter/hint_apply"_i18n, brls::BUTTON_B, [this](brls::View* view) {
        this->applyAndDismiss();
        return true;
    }, false, false, brls::SOUND_BACK);

    this->addGestureRecognizer(new brls::TapGestureRecognizer([this](brls::TapGestureStatus status, brls::Sound* soundToPlay) {
        if (status.state == brls::GestureState::END) {
            if (this->content && !this->content->getFrame().pointInside(status.position)) {
                this->applyAndDismiss();
            }
        }
    }));
}

void MultiSelectDialog::open(const std::string& title,
                             const std::vector<MultiSelectItem>& items,
                             const std::vector<std::string>& selectedIds,
                             std::function<void(const std::vector<std::string>&)> onApply) {
    auto* dialog = new MultiSelectDialog(title, items, selectedIds, std::move(onApply));
    brls::Application::pushActivity(new brls::Activity(dialog));
}

int MultiSelectDialog::numberOfRows(brls::RecyclerFrame* recycler, int section) {
    return static_cast<int>(items_.size());
}

brls::RecyclerCell* MultiSelectDialog::cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) {
    auto* cell = dynamic_cast<brls::RadioCell*>(recycler->dequeueReusableCell("Cell"));
    if (!cell) return nullptr;

    int row = index.row;
    if (row >= 0 && row < static_cast<int>(items_.size())) {
        cell->title->setText(items_[row].displayName);
        bool isSelected = false;
        if (row == 0) {
            isSelected = selectedIds_.empty();
        } else {
            isSelected = (selectedIds_.count(items_[row].id) > 0);
        }
        cell->setSelected(isSelected);
    }
    return cell;
}

void MultiSelectDialog::didSelectRowAt(brls::RecyclerFrame* recycler, brls::IndexPath index) {
    int row = index.row;
    if (row < 0 || row >= static_cast<int>(items_.size())) return;

    // Button A / Click: single selection only. Clears all other items, selects this one and closes.
    selectedIds_.clear();
    if (row > 0) {
        selectedIds_.insert(items_[row].id);
    }
    applyAndDismiss();
}

void MultiSelectDialog::toggleItem(int row) {
    if (row < 0 || row >= static_cast<int>(items_.size())) return;

    if (row == 0) {
        selectedIds_.clear();
    } else {
        const std::string& id = items_[row].id;
        if (selectedIds_.count(id)) {
            selectedIds_.erase(id);
        } else {
            selectedIds_.insert(id);
        }
    }
    updateVisibleCheckmarks();
}

void MultiSelectDialog::updateVisibleCheckmarks() {
    for (auto* cell : allocatedCells_) {
        int row = cell->getIndexPath().row;
        if (row < 0 || row >= static_cast<int>(items_.size())) continue;

        bool isSelected = false;
        if (row == 0) {
            isSelected = selectedIds_.empty();
        } else {
            isSelected = (selectedIds_.count(items_[row].id) > 0);
        }
        cell->setSelected(isSelected);
    }
}

void MultiSelectDialog::applyAndDismiss() {
    if (dismissed_) return;
    dismissed_ = true;

    std::vector<std::string> result;
    for (const auto& item : items_) {
        if (item.id.empty()) continue;
        if (selectedIds_.count(item.id)) {
            result.push_back(item.id);
        }
    }

    if (onApply_) {
        onApply_(result);
    }

    brls::Application::popActivity(brls::TransitionAnimation::FADE);
}

void MultiSelectDialog::show(std::function<void()> cb, bool animate, float animationDuration) {
    if (animate) {
        content->setTranslationY(30.0f);

        showOffset.stop();
        showOffset.reset(30.0f);
        showOffset.addStep(0, animationDuration, brls::EasingFunction::quadraticOut);
        showOffset.setTickCallback([this] { this->offsetTick(); });
        showOffset.start();
    }

    Box::show(cb, animate, animationDuration);

    if (animate) {
        alpha.stop();
        alpha.reset(1);

        applet->alpha.stop();
        applet->alpha.reset(0);
        applet->alpha.addStep(1, animationDuration, brls::EasingFunction::quadraticOut);
        applet->alpha.start();
    }
}

void MultiSelectDialog::hide(std::function<void()> cb, bool animated, float animationDuration) {
    if (animated) {
        alpha.stop();
        alpha.reset(0);

        applet->alpha.stop();
        applet->alpha.reset(1);
        applet->alpha.addStep(0, animationDuration, brls::EasingFunction::quadraticOut);
        applet->alpha.start();
    }

    Box::hide(cb, animated, animationDuration);
}

brls::View* MultiSelectDialog::getParentNavigationDecision(brls::View* from, brls::View* newFocus, brls::FocusDirection direction) {
    return Box::getParentNavigationDecision(from, newFocus, direction);
}

float MultiSelectDialog::getShowAnimationDuration(brls::TransitionAnimation animation) {
    return brls::View::getShowAnimationDuration(animation) / 2;
}

void MultiSelectDialog::offsetTick() {
    content->setTranslationY(showOffset);
}

} // namespace ui
