#pragma once

#include <borealis.hpp>
#include <string>
#include <vector>

namespace ui {

class BackgroundFilePicker : public brls::Activity {
public:
    BackgroundFilePicker(const std::string& initialDir = "");
    ~BackgroundFilePicker() override = default;

    brls::View* createContentView() override;
    void onContentAvailable() override;

private:
    void navigateTo(const std::string& path);
    void refreshList();
    void onItemSelected(const std::string& path, bool isDir);

    std::string currentDir_;
    brls::Box* rootBox_ = nullptr;
    brls::Label* pathLabel_ = nullptr;
    brls::Box* listContainer_ = nullptr;
    brls::ScrollingFrame* scroll_ = nullptr;
};

} // namespace ui
