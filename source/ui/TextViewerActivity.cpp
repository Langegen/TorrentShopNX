#include "TextViewerActivity.hpp"
#include "../utils/file_ops.h"
#include "../utils/log.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

namespace ui {

namespace {

bool readTextFileContent(const std::string& path, std::string& outText, size_t& outLines, uint64_t& outTotalSize, bool& outTruncated, std::string& outError) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        outError = "Файл не найден: " + path;
        return false;
    }

    outTotalSize = std::filesystem::file_size(path, ec);

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        outError = "Не удалось открыть файл для чтения";
        return false;
    }

    constexpr size_t MAX_READ_BYTES = 512 * 1024; // 512 KB
    size_t bytesToRead = std::min<size_t>(static_cast<size_t>(outTotalSize), MAX_READ_BYTES);
    outTruncated = (outTotalSize > MAX_READ_BYTES);

    outText.resize(bytesToRead);
    file.read(&outText[0], bytesToRead);
    size_t bytesRead = static_cast<size_t>(file.gcount());
    outText.resize(bytesRead);

    // Sanitize binary zeroes into spaces so Borealis doesn't truncate early
    outLines = 1;
    for (char& ch : outText) {
        if (ch == '\0') {
            ch = ' ';
        } else if (ch == '\n') {
            outLines++;
        }
    }

    if (outTruncated) {
        outText += "\n\n--- [Файл слишком большой. Показаны первые 512 КБ] ---";
    }

    return true;
}

} // namespace

TextViewerActivity::TextViewerActivity(const std::string& filePath, const std::string& fileName)
    : filePath_(filePath), fileName_(fileName) {
    if (fileName_.empty()) {
        std::filesystem::path p(filePath_);
        fileName_ = p.filename().generic_string();
    }
}

brls::View* TextViewerActivity::createContentView() {
    // Root container (Dark console theme)
    auto* root = new brls::Box();
    root->setAxis(brls::Axis::COLUMN);
    root->setWidth(brls::Application::windowWidth);
    root->setHeight(brls::Application::windowHeight);
    root->setBackgroundColor(nvgRGBA(18, 20, 24, 255));
    root->setPadding(18.0f, 24.0f, 16.0f, 24.0f);

    // Read content
    std::string textContent;
    size_t lineCount = 0;
    uint64_t totalSize = 0;
    bool truncated = false;
    std::string err;
    bool ok = readTextFileContent(filePath_, textContent, lineCount, totalSize, truncated, err);

    // ── Header Card ────────────────────────────────────────────────────────
    auto* headerBox = new brls::Box();
    headerBox->setAxis(brls::Axis::ROW);
    headerBox->setAlignItems(brls::AlignItems::CENTER);
    headerBox->setMarginBottom(12.0f);
    headerBox->setPaddingBottom(12.0f);
    headerBox->setLineBottom(1.0f);
    headerBox->setLineColor(nvgRGBA(0, 224, 165, 75));

    // Icon Badge
    auto* iconBadge = new brls::Box();
    iconBadge->setWidth(42.0f);
    iconBadge->setHeight(42.0f);
    iconBadge->setCornerRadius(8.0f);
    iconBadge->setJustifyContent(brls::JustifyContent::CENTER);
    iconBadge->setAlignItems(brls::AlignItems::CENTER);
    iconBadge->setMarginRight(14.0f);
    iconBadge->setBackgroundColor(nvgRGBA(0, 224, 165, 30));

    auto* badgeIcon = new brls::Label();
    badgeIcon->setText("\uE873"); // Material document / article icon
    badgeIcon->setFontSize(22.0f);
    badgeIcon->setTextColor(nvgRGB(0, 224, 165));
    iconBadge->addView(badgeIcon);
    headerBox->addView(iconBadge);

    auto* headerCol = new brls::Box();
    headerCol->setAxis(brls::Axis::COLUMN);
    headerCol->setGrow(1.0f);

    auto* titleLbl = new brls::Label();
    titleLbl->setText(fileName_);
    titleLbl->setFontSize(20.0f);
    titleLbl->setTextColor(nvgRGB(255, 255, 255));
    titleLbl->setSingleLine(true);
    headerCol->addView(titleLbl);

    std::string metaStr = filePath_ + " · " + util::formatFileSize(totalSize);
    if (ok) {
        metaStr += " · " + std::to_string(lineCount) + " строк";
    }
    auto* subLbl = new brls::Label();
    subLbl->setText(metaStr);
    subLbl->setFontSize(13.0f);
    subLbl->setTextColor(nvgRGBA(180, 190, 205, 220));
    subLbl->setSingleLine(true);
    headerCol->addView(subLbl);

    headerBox->addView(headerCol);
    root->addView(headerBox);

    // ── Content Area: Scrolling Frame ─────────────────────────────────────
    scrollingFrame_ = new brls::ScrollingFrame();
    scrollingFrame_->setGrow(1.0f);
    scrollingFrame_->setScrollingBehavior(brls::ScrollingBehavior::NATURAL);
    scrollingFrame_->setFocusable(true);

    auto* textContainer = new brls::Box();
    textContainer->setAxis(brls::Axis::COLUMN);
    textContainer->setWidthPercentage(100.0f);
    textContainer->setPadding(14.0f, 18.0f, 20.0f, 18.0f);
    textContainer->setBackgroundColor(nvgRGBA(26, 28, 34, 230));
    textContainer->setCornerRadius(8.0f);

    textLabel_ = new brls::Label();
    textLabel_->setWidthPercentage(100.0f);
    textLabel_->setFontSize(15.0f);
    textLabel_->setSingleLine(false);
    textLabel_->setHorizontalAlign(brls::HorizontalAlign::LEFT);
    textLabel_->setVerticalAlign(brls::VerticalAlign::TOP);

    if (!ok) {
        textLabel_->setTextColor(nvgRGB(255, 100, 100));
        textLabel_->setText("Ошибка чтения файла: " + err);
    } else if (textContent.empty()) {
        textLabel_->setTextColor(nvgRGB(140, 145, 155));
        textLabel_->setText("<Файл пуст>");
    } else {
        textLabel_->setTextColor(nvgRGB(228, 232, 240));
        textLabel_->setText(textContent);
    }

    textContainer->addView(textLabel_);
    scrollingFrame_->setContentView(textContainer);
    root->addView(scrollingFrame_);

    return root;
}

void TextViewerActivity::onContentAvailable() {
    // Register actions for navigation
    this->registerAction("hints/back"_i18n, brls::ControllerButton::BUTTON_B, [](brls::View* view) {
        brls::Application::popActivity();
        return true;
    });

    this->registerAction("Стр. Вверх", brls::ControllerButton::BUTTON_LB, [this](brls::View* view) {
        if (scrollingFrame_) {
            float cur = scrollingFrame_->getContentOffsetY();
            scrollingFrame_->setContentOffsetY(std::max(0.0f, cur - 400.0f), true);
        }
        return true;
    });

    this->registerAction("Стр. Вниз", brls::ControllerButton::BUTTON_RB, [this](brls::View* view) {
        if (scrollingFrame_) {
            float cur = scrollingFrame_->getContentOffsetY();
            scrollingFrame_->setContentOffsetY(cur + 400.0f, true);
        }
        return true;
    });

    // Keyboard navigation
    this->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_ESCAPE), [](brls::View* view) {
        brls::Application::popActivity();
        return true;
    });

    this->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_PAGE_UP), [this](brls::View* view) {
        if (scrollingFrame_) {
            float cur = scrollingFrame_->getContentOffsetY();
            scrollingFrame_->setContentOffsetY(std::max(0.0f, cur - 400.0f), true);
        }
        return true;
    });

    this->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_PAGE_DOWN), [this](brls::View* view) {
        if (scrollingFrame_) {
            float cur = scrollingFrame_->getContentOffsetY();
            scrollingFrame_->setContentOffsetY(cur + 400.0f, true);
        }
        return true;
    });

    this->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_HOME), [this](brls::View* view) {
        if (scrollingFrame_) {
            scrollingFrame_->resetScrollToTop();
        }
        return true;
    });

    if (scrollingFrame_) {
        brls::Application::giveFocus(scrollingFrame_);
    }
}

} // namespace ui
