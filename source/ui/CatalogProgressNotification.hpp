#pragma once
#include <borealis.hpp>
#include <unistd.h>
#include <memory>

namespace ui {

class CatalogProgressNotification : public brls::Box {
public:
    CatalogProgressNotification() {
        aliveToken_ = std::make_shared<bool>(true);
        this->setAxis(brls::Axis::COLUMN);
        this->setWidth(300);
        this->setPadding(12);
        this->setMarginTop(10);
        this->setMarginRight(15);
        this->setCornerRadius(10);
        this->setBackgroundColor(nvgRGBA(30, 30, 35, 235)); // Dark translucent card background

        // Title header
        headerLabel = new brls::Label();
        headerLabel->setText("app/catalog/updating_db"_i18n);
        headerLabel->setFontSize(14);
        headerLabel->setTextColor(nvgRGB(255, 255, 255));
        headerLabel->setMarginBottom(6);
        this->addView(headerLabel);

        // Progress bar container & fill
        progressBg = new brls::Box();
        progressBg->setWidth(276); // 300 - 24 padding
        progressBg->setHeight(8);
        progressBg->setCornerRadius(4);
        progressBg->setBackgroundColor(nvgRGB(60, 60, 65));
        progressBg->setMarginBottom(6);

        progressFill = new brls::Box();
        progressFill->setWidth(0);
        progressFill->setHeight(8);
        progressFill->setCornerRadius(4);
        progressFill->setBackgroundColor(nvgRGB(33, 150, 243)); // Blue fill

        progressBg->addView(progressFill);
        this->addView(progressBg);

        // Status text label
        statusLabel = new brls::Label();
        statusLabel->setText("app/catalog/connecting"_i18n);
        statusLabel->setFontSize(12);
        statusLabel->setTextColor(nvgRGB(170, 170, 180));
        this->addView(statusLabel);
    }

    ~CatalogProgressNotification() override {
        if (aliveToken_) {
            *aliveToken_ = false;
        }
    }

    std::shared_ptr<bool> getAliveToken() const { return aliveToken_; }

    void updateProgress(float percent, const std::string& statusText) {
        if (percent < 0.0f) percent = 0.0f;
        if (percent > 100.0f) percent = 100.0f;

        float fillWidth = (276.0f * percent) / 100.0f;
        progressFill->setWidth(fillWidth);
        statusLabel->setText(statusText);
    }

    void setCompleted(const std::string& msg) {
        progressFill->setWidth(276);
        progressFill->setBackgroundColor(nvgRGB(76, 175, 80)); // Green fill on complete
        headerLabel->setText("app/catalog/db_updated"_i18n);
        statusLabel->setText(msg);

        // Auto dismiss after 3 seconds with cancellation token
        auto token = aliveToken_;
        brls::async([this, token]() {
            for (int i = 0; i < 30; ++i) {
                usleep(100000); // 100ms chunks
                if (!token || !*token) return;
            }
            brls::sync([this, token]() {
                if (!token || !*token) return;
                if (this->hasParent()) {
                    this->getParent()->removeView(this);
                }
            });
        });
    }

    void setFailed(const std::string& msg) {
        progressFill->setBackgroundColor(nvgRGB(244, 67, 54)); // Red fill on error
        headerLabel->setText("app/catalog/update_error"_i18n);
        statusLabel->setText(msg);

        // Auto dismiss after 4 seconds with cancellation token
        auto token = aliveToken_;
        brls::async([this, token]() {
            for (int i = 0; i < 40; ++i) {
                usleep(100000); // 100ms chunks
                if (!token || !*token) return;
            }
            brls::sync([this, token]() {
                if (!token || !*token) return;
                if (this->hasParent()) {
                    this->getParent()->removeView(this);
                }
            });
        });
    }

private:
    std::shared_ptr<bool> aliveToken_;
    brls::Label* headerLabel = nullptr;
    brls::Box* progressBg = nullptr;
    brls::Box* progressFill = nullptr;
    brls::Label* statusLabel = nullptr;
};

} // namespace ui
