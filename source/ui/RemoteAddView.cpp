#include "RemoteAddView.hpp"
#include "FileSelectView.hpp"
#include "../GameData.hpp"
#include "../utils/log.h"
#include <switch.h>
#include <arpa/inet.h>

#if __has_include(<libtorrent/torrent_info.hpp>)
#include <libtorrent/torrent_info.hpp>
#endif

namespace ui {

RemoteAddView::RemoteAddView() {
}

RemoteAddView::~RemoteAddView() {
    stopServer();
    if (pollTimer) {
        pollTimer->stop();
        delete pollTimer;
    }
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
    struct in_addr addr;
    addr.s_addr = gethostid();
    if (addr.s_addr == 0) {
        return "127.0.0.1"; // Fallback or not connected
    }
    return std::string(inet_ntoa(addr));
}

void RemoteAddView::startServer() {
    if (serverRunning) return;
    
    svr.Get("/", [](const httplib::Request& req, httplib::Response& res) {
        std::string html = R"(
            <!DOCTYPE html>
            <html>
            <head>
                <meta charset="utf-8">
                <meta name="viewport" content="width=device-width, initial-scale=1">
                <title>TorrentShopNX - Добавить раздачу</title>
                <style>
                    body { font-family: sans-serif; background: #222; color: #fff; padding: 20px; max-width: 600px; margin: 0 auto; }
                    h2 { text-align: center; }
                    .card { background: #333; padding: 20px; border-radius: 8px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); margin-bottom: 20px; }
                    input[type="text"], input[type="file"] { width: 100%; padding: 10px; margin: 10px 0; border-radius: 4px; border: 1px solid #555; background: #444; color: #fff; box-sizing: border-box; }
                    button { width: 100%; padding: 12px; background: #007bff; color: white; border: none; border-radius: 4px; font-size: 16px; cursor: pointer; }
                    button:hover { background: #0056b3; }
                </style>
            </head>
            <body>
                <h2>TorrentShopNX</h2>
                <div class="card">
                    <h3>Вставить Magnet-ссылку</h3>
                    <form action="/magnet" method="post">
                        <input type="text" name="magnet" placeholder="magnet:?xt=urn:btih:..." required>
                        <button type="submit">Отправить</button>
                    </form>
                </div>
                <div class="card">
                    <h3>Загрузить .torrent файл</h3>
                    <form action="/torrent" method="post" enctype="multipart/form-data">
                        <input type="file" name="file" accept=".torrent" required>
                        <button type="submit">Загрузить</button>
                    </form>
                </div>
            </body>
            </html>
        )";
        res.set_content(html, "text/html");
    });

    svr.Post("/magnet", [this](const httplib::Request& req, httplib::Response& res) {
        if (req.has_param("magnet")) {
            this->receivedMagnet = req.get_param_value("magnet");
            this->fileReceived = true;
            res.set_content("Успешно! Посмотрите на экран приставки.", "text/plain");
        } else {
            res.set_content("Ошибка: нет ссылки", "text/plain");
        }
    });

    svr.Post("/torrent", [this](const httplib::Request& req, httplib::Response& res) {
        if (req.form.has_file("file")) {
            const auto& file = req.form.get_file("file");
            
            // Save temporary torrent file
            std::string tempPath = "sdmc:/switch/TorrentShopNX/temp_upload.torrent";
            FILE* f = fopen(tempPath.c_str(), "wb");
            if (f) {
                fwrite(file.content.data(), 1, file.content.size(), f);
                fclose(f);
                
                // Parse magnet from it using libtorrent
                #if __has_include(<libtorrent/torrent_info.hpp>)
                lt::error_code ec;
                lt::torrent_info ti(tempPath, ec);
                if (!ec) {
                    char hex[41];
                    auto hash = ti.info_hash().to_string();
                    for (int i=0; i<20; ++i) {
                        sprintf(hex + (i*2), "%02x", (unsigned char)hash[i]);
                    }
                    this->receivedMagnet = "magnet:?xt=urn:btih:" + std::string(hex) + "&dn=" + ti.name();
                    this->fileReceived = true;
                    res.set_content("Торрент файл успешно обработан! Посмотрите на экран приставки.", "text/plain");
                } else {
                    res.set_content("Ошибка парсинга торрента: " + ec.message(), "text/plain");
                }
                #else
                res.set_content("Ошибка: libtorrent не подключен в этой сборке", "text/plain");
                #endif
            } else {
                res.set_content("Ошибка сохранения файла", "text/plain");
            }
        } else {
            res.set_content("Ошибка: файл не найден", "text/plain");
        }
    });

    serverRunning = true;
    serverThread = std::thread([this]() {
        util::logLine("RemoteAddView: server thread started");
        svr.listen("0.0.0.0", 8080);
        util::logLine("RemoteAddView: server thread stopped");
    });
    
    pollTimer = new brls::RepeatingTimer();
    pollTimer->setPeriod(500);
    pollTimer->setCallback([this]() {
        if (this->fileReceived) {
            this->fileReceived = false;
            
            Game game;
            game.title = "Пользовательская раздача";
            game.magnet = this->receivedMagnet;
            game.size = "Неизвестно";
            game.description = "Раздача добавлена по сети";
            
            util::logLine("RemoteAddView: received magnet, pushing FileSelectView");
            brls::Application::pushActivity(new ui::FileSelectView(game));
        }
    });
    pollTimer->start();
}

void RemoteAddView::stopServer() {
    if (serverRunning) {
        svr.stop();
        if (serverThread.joinable()) {
            serverThread.join();
        }
        serverRunning = false;
    }
}

} // namespace ui
