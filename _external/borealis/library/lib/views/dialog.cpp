/*
    Copyright 2019-2021 natinusala
    Copyright 2021 XITRIX

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <borealis/core/application.hpp>
#include <borealis/core/i18n.hpp>
#include <borealis/views/dialog.hpp>
#include <borealis/views/label.hpp>
#include <borealis/core/touch/tap_gesture.hpp>

using namespace brls::literals;

// TODO: different open animation?

namespace brls
{

const std::string dialogXML = R"xml(
    <brls:Box
        width="auto"
        height="auto"
        axis="column"
        justifyContent="center"
        alignItems="center"
        backgroundColor="@theme/brls/backdrop">

        <brls:AppletFrame
            id="brls/dialog/applet"
            width="720"
            height="auto"
            headerHidden="true"
            footerHidden="true"
            cornerRadius="4"
            backgroundColor="@theme/brls/background">

            <brls:Box
                width="auto"
                height="auto"
                grow="1"
                axis="column">

                <brls:Box
                    id="brls/dialog/container"
                    width="auto"
                    height="auto"
                    grow="1"
                    axis="column"/>

                <brls:Box
                    id="brls/dialog/buttonBox"
                    width="auto"
                    height="auto"
                    axis="column"/>
            
            </brls:Box>

        </brls:AppletFrame>

    </brls:Box>
)xml";

Dialog::Dialog(Box* contentView)
{
    this->inflateFromXMLString(dialogXML);
    container->addView(contentView);

    appletFrame->registerAction(
        "hints/back"_i18n, BUTTON_B, [this](View* view) {
            if (cancelable)
                this->dismiss();
            return cancelable;
        },
        false, false, SOUND_BACK);

    this->addGestureRecognizer(new TapGestureRecognizer([this](TapGestureStatus status, Sound* soundToPlay) {
        if (status.state == GestureState::END && this->cancelable) {
            if (this->appletFrame && !this->appletFrame->getFrame().pointInside(status.position)) {
                this->dismiss();
            }
        }
    }));
}

Dialog::Dialog(std::string text)
{
    Style style = Application::getStyle();

    Label* label = new Label();
    label->setText(text);
    label->setFontSize(style["brls/dialog/fontSize"]);
    label->setHorizontalAlign(HorizontalAlign::CENTER);
    label->setSingleLine(false);
    label->setWidth(720.0f - 2.0f * style["brls/dialog/paddingLeftRight"]);

    Box* box = new Box();
    box->addView(label);
    box->setAlignItems(AlignItems::CENTER);
    box->setJustifyContent(JustifyContent::CENTER);
    box->setPadding(style["brls/dialog/paddingTopBottom"], style["brls/dialog/paddingLeftRight"], style["brls/dialog/paddingTopBottom"], style["brls/dialog/paddingLeftRight"]);

    this->inflateFromXMLString(dialogXML);
    container->addView(box);

    appletFrame->registerAction(
        "hints/back"_i18n, BUTTON_B, [this](View* view) {
            if (cancelable)
                this->dismiss();
            return cancelable;
        },
        false, false, SOUND_BACK);

    this->addGestureRecognizer(new TapGestureRecognizer([this](TapGestureStatus status, Sound* soundToPlay) {
        if (status.state == GestureState::END && this->cancelable) {
            if (this->appletFrame && !this->appletFrame->getFrame().pointInside(status.position)) {
                this->dismiss();
            }
        }
    }));
}

static size_t utf8CharCount(const std::string& str)
{
    size_t res = 0, inc = 0;
    while (inc < str.length())
    {
        if (str[inc] & 0x80)
            if (str[inc] & 0x20)
                if (str[inc] & 0x10)
                    inc += 4;
                else
                    inc += 3;
            else
                inc += 2;
        else
            inc += 1;
        res++;
    }
    return res;
}

void Dialog::addButton(std::string label, VoidEvent::Callback cb)
{
    DialogButton* button = new DialogButton();
    button->label        = label;
    button->cb           = cb;

    this->buttons.push_back(button);

    this->rebuildButtons();
}

void Dialog::open()
{
    Application::pushActivity(new Activity(this));
}

void Dialog::close(std::function<void(void)> cb)
{
    Box::dismiss(cb);
}

void Dialog::setCancelable(bool cancelable)
{
    this->cancelable = cancelable;
}

void Dialog::rebuildButtons()
{
    if (!buttonBox)
        return;

    buttonBox->clearViews(true);
    button1 = nullptr;
    button2 = nullptr;
    button3 = nullptr;
    button2separator = nullptr;
    button3separator = nullptr;

    if (this->buttons.empty())
        return;

    Style style = Application::getStyle();
    Theme theme = Application::getTheme();

    auto createBtn = [this](DialogButton* b) -> Button* {
        Theme theme = Application::getTheme();
        Style style = Application::getStyle();
        Button* btn = new Button();
        btn->setHeight(72);
        btn->setStyle(&BUTTONSTYLE_BORDERLESS);
        btn->setTextColor(theme["brls/accent"]);
        btn->setFontSize(style["brls/dialog/fontSize"]);
        btn->setText(b->label);
        btn->registerClickAction([this, b](View* view) {
            buttonClick(b);
            return true;
        });
        return btn;
    };

    auto createSep = []() -> Rectangle* {
        Theme theme = Application::getTheme();
        Rectangle* sep = new Rectangle();
        sep->setHeight(2);
        sep->setColor(theme["brls/sidebar/separator"]);
        return sep;
    };

    if (this->buttons.size() == 1)
    {
        button1 = createBtn(buttons[0]);
        buttonBox->addView(createSep());
        buttonBox->addView(button1);
        setLastFocusedView(button1);
        return;
    }

    if (this->buttons.size() == 2)
    {
        bool fitSideBySide = true;
        NVGcontext* vg = Application::getNVGContext();
        for (DialogButton* b : this->buttons)
        {
            if (utf8CharCount(b->label) > 16)
            {
                fitSideBySide = false;
                break;
            }

            if (vg)
            {
                int font = Application::getDefaultFont();
                if (font != FONT_INVALID)
                {
                    nvgFontSize(vg, style["brls/dialog/fontSize"]);
                    nvgFontFaceId(vg, font);
                    float bounds[4];
                    nvgTextBounds(vg, 0, 0, b->label.c_str(), nullptr, bounds);
                    float requiredWidth = bounds[2] - bounds[0];
                    if (requiredWidth > 260.0f)
                    {
                        fitSideBySide = false;
                        break;
                    }
                }
            }
        }

        if (fitSideBySide)
        {
            Box* row = new Box();
            row->setAxis(Axis::ROW);
            row->setHeight(72);
            row->setJustifyContent(JustifyContent::SPACE_EVENLY);
            row->setAlignItems(AlignItems::STRETCH);

            button1 = createBtn(buttons[0]);
            button1->setWidth(0);
            button1->setGrow(1);

            button2separator = new Rectangle();
            button2separator->setWidth(2);
            button2separator->setColor(theme["brls/sidebar/separator"]);

            button2 = createBtn(buttons[1]);
            button2->setWidth(0);
            button2->setGrow(1);

            row->addView(button1);
            row->addView(button2separator);
            row->addView(button2);

            buttonBox->addView(createSep());
            buttonBox->addView(row);
            setLastFocusedView(button1);
            return;
        }
    }

    // 3 or more buttons, or 2 buttons with long text: stack vertically
    std::vector<Button*> createdBtns;
    for (size_t i = 0; i < this->buttons.size(); i++)
    {
        Rectangle* sep = createSep();
        buttonBox->addView(sep);

        Button* btn = createBtn(buttons[i]);
        buttonBox->addView(btn);
        createdBtns.push_back(btn);

        if (i == 0)
            button1 = btn;
        else if (i == 1)
        {
            button2 = btn;
            button2separator = sep;
        }
        else if (i == 2)
        {
            button3 = btn;
            button3separator = sep;
        }
    }

    if (!createdBtns.empty())
        setLastFocusedView(createdBtns[0]);
}

void Dialog::buttonClick(DialogButton* button)
{
    dismiss([button] {
        button->cb();
    });
}

AppletFrame* Dialog::getAppletFrame()
{
    return appletFrame;
}

Dialog::~Dialog()
{
    for(auto& i: this->buttons){
        delete i;
    }
}

} // namespace brls
