#pragma once

#include <borealis.hpp>
#include <string>
#include <thread>
#include <atomic>
#include <httplib.h>
#include "QrCodeView.hpp"

namespace ui {

class RemoteAddView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("remote_add_view.xml");

    RemoteAddView();
    ~RemoteAddView() override;

    void onContentAvailable() override;
    void willAppear(bool resetState = false) override;
    void willDisappear(bool resetState = false) override;

    static std::string getLocalIpAddress();

private:
    BRLS_BIND(brls::Label, lblUrl, "lblUrl");
    BRLS_BIND(QrCodeView, qrView, "qrView");

    void startServer();
    void stopServer();

    std::thread serverThread;
    httplib::Server svr;
    std::atomic<bool> serverRunning{false};
    std::atomic<bool> fileReceived{false};
    std::string receivedMagnet;
    brls::RepeatingTimer* pollTimer = nullptr;
};

} // namespace ui
