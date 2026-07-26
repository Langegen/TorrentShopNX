#include "AppletWarningView.hpp"

namespace ui {

AppletWarningView::AppletWarningView() = default;

void AppletWarningView::onContentAvailable() {
    btnExit->registerClickAction([](brls::View* view) {
        brls::Application::quit();
        return true;
    });
    brls::Application::giveFocus(btnExit);
}

} // namespace ui
