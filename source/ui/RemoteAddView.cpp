#include "RemoteAddView.hpp"
#include "FileSelectView.hpp"
#include "../GameData.hpp"
#include "../utils/log.h"
#include "../utils/app_paths.h"
#include "../engine/torrent_meta.h"
#ifdef __SWITCH__
#include <switch.h>
#include <arpa/inet.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#endif
#include <cstdio>

namespace ui {

RemoteAddView::RemoteAddView() {
}

RemoteAddView::~RemoteAddView() {
    stopServer();
}

void RemoteAddView::onContentAvailable() {
    util::logLine("RemoteAddView: onContentAvailable");
    
    std::string ip = getLocalIpAddress();
    std::string url = "http://" + ip + ":8080/";
    
    lblUrl->setText(url);
    qrView->setContent(url);
}

void RemoteAddView::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    startServer();
}

void RemoteAddView::willDisappear(bool resetState) {
    brls::Activity::willDisappear(resetState);
    stopServer();
}

std::string RemoteAddView::getLocalIpAddress() {
#ifdef __SWITCH__
    struct in_addr addr;
    addr.s_addr = gethostid();
    if (addr.s_addr == 0) {
        return "127.0.0.1"; // Fallback or not connected
    }
    return std::string(inet_ntoa(addr));
#elif defined(_WIN32)
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        struct hostent* host = gethostbyname(hostname);
        if (host && host->h_addr_list && host->h_addr_list[0]) {
            struct in_addr addr;
            memcpy(&addr, host->h_addr_list[0], sizeof(struct in_addr));
            return std::string(inet_ntoa(addr));
        }
    }
    return "127.0.0.1";
#else
    return "127.0.0.1";
#endif
}

void RemoteAddView::startServer() {
    if (serverRunning) return;
    
    svr.Get("/", [](const httplib::Request& req, httplib::Response& res) {
        std::string htmlTitle = "app/remote/web_title"_i18n;
        std::string htmlInsert = "app/remote/web_insert_magnet"_i18n;
        std::string htmlSend = "app/remote/web_send_btn"_i18n;
        std::string htmlUpload = "app/remote/web_upload_torrent"_i18n;
        std::string htmlUploadBtn = "app/remote/web_upload_btn"_i18n;

        std::string html = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
            "<title>" + htmlTitle + "</title>"
            "<style>"
            "body { font-family: sans-serif; background: #222; color: #fff; padding: 20px; max-width: 600px; margin: 0 auto; }"
            "h2 { text-align: center; }"
            ".card { background: #333; padding: 20px; border-radius: 8px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); margin-bottom: 20px; }"
            "input[type=\"text\"], input[type=\"file\"] { width: 100%; padding: 10px; margin: 10px 0; border-radius: 4px; border: 1px solid #555; background: #444; color: #fff; box-sizing: border-box; }"
            "button { width: 100%; padding: 12px; background: #007bff; color: white; border: none; border-radius: 4px; font-size: 16px; cursor: pointer; }"
            "button:hover { background: #0056b3; }"
            "</style></head><body>"
            "<h2>TorrentShopNX</h2>"
            "<div class=\"card\">"
            "<h3>" + htmlInsert + "</h3>"
            "<form action=\"/magnet\" method=\"post\">"
            "<input type=\"text\" name=\"magnet\" placeholder=\"magnet:?xt=urn:btih:...\" required>"
            "<button type=\"submit\">" + htmlSend + "</button>"
            "</form></div>"
            "<div class=\"card\">"
            "<h3>" + htmlUpload + "</h3>"
            "<form action=\"/torrent\" method=\"post\" enctype=\"multipart/form-data\">"
            "<input type=\"file\" name=\"file\" accept=\".torrent\" required>"
            "<button type=\"submit\">" + htmlUploadBtn + "</button>"
            "</form></div></body></html>";
        res.set_content(html, "text/html");
    });

    svr.Post("/magnet", [this](const httplib::Request& req, httplib::Response& res) {
        if (req.has_param("magnet")) {
            this->receivedMagnet = req.get_param_value("magnet");
            this->fileReceived = true;
            res.set_content("app/remote/web_magnet_success"_i18n, "text/plain; charset=utf-8");
        } else {
            res.set_content("app/remote/web_no_link"_i18n, "text/plain; charset=utf-8");
        }
    });

    svr.Post("/torrent", [this](const httplib::Request& req, httplib::Response& res) {
        if (req.form.has_file("file")) {
            const auto& file = req.form.get_file("file");

            // Save temporary torrent file
            std::string tempPath = TSNX_TEMP_UPLOAD;
            FILE* f = fopen(tempPath.c_str(), "wb");
            if (f) {
                fwrite(file.content.data(), 1, file.content.size(), f);
                fclose(f);

                // Parse info hash and name using the custom engine's metadata parser
                torrent_meta t;
                char err[256] = {0};
                if (torrent_load(&t, tempPath.c_str(), err, sizeof(err)) == 0) {
                    char hex[41];
                    static const char digits[] = "0123456789abcdef";
                    for (int i = 0; i < 20; ++i) {
                        hex[i * 2]     = digits[(t.info_hash[i] >> 4) & 0xF];
                        hex[i * 2 + 1] = digits[t.info_hash[i] & 0xF];
                    }
                    hex[40] = '\0';
                    std::string name = t.name[0] ? t.name : "uploaded";
                    this->receivedMagnet = "magnet:?xt=urn:btih:" + std::string(hex) + "&dn=" + name;
                    this->fileReceived = true;
                    torrent_unload(&t);
                    res.set_content("app/remote/web_torrent_success"_i18n, "text/plain; charset=utf-8");
                } else {
                    res.set_content(brls::getStr("app/remote/web_torrent_parse_error", err), "text/plain; charset=utf-8");
                }
            } else {
                res.set_content("app/remote/web_torrent_save_error"_i18n, "text/plain; charset=utf-8");
            }
        } else {
            res.set_content("app/remote/web_file_not_found"_i18n, "text/plain; charset=utf-8");
        }
    });

    serverRunning = true;
    serverThread = std::thread([this]() {
        util::logLine("RemoteAddView: server thread started");
        svr.listen("0.0.0.0", 8080);
        util::logLine("RemoteAddView: server thread stopped");
    });
    
    if (pollTimer) {
        pollTimer->stop();
        delete pollTimer;
        pollTimer = nullptr;
    }

    pollTimer = new brls::RepeatingTimer();
    pollTimer->setPeriod(500);
    pollTimer->setCallback([this]() {
        if (this->fileReceived) {
            this->fileReceived = false;
            
            Game game;
            game.title = "app/remote/user_upload_title"_i18n;
            game.magnet = this->receivedMagnet;
            game.size = "app/common/unknown"_i18n;
            game.description = "app/remote/user_upload_desc"_i18n;
            
            util::logLine("RemoteAddView: received magnet, pushing FileSelectView");
            brls::Application::pushActivity(new ui::FileSelectView(game));
        }
    });
    pollTimer->start();
}

void RemoteAddView::stopServer() {
    if (pollTimer) {
        pollTimer->stop();
        delete pollTimer;
        pollTimer = nullptr;
    }
    if (serverRunning) {
        svr.stop();
        if (serverThread.joinable()) {
            serverThread.join();
        }
        serverRunning = false;
    }
}

} // namespace ui
