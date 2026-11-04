#include "BugReportFlow.hpp"

#include "QrCodeView.hpp"
#include "RemoteAddView.hpp"
#include "../config/config.h"
#include "../utils/bug_report.h"
#include "../utils/log.h"

#include <httplib.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>

namespace ui::bugreport {

namespace {

constexpr int kSharePort = 8081;

std::string g_message;
// One report at a time: building twice in parallel would race on the
// staging folder.
std::atomic<bool> g_busy{false};

// "sdmc:/switch/..." -> "/switch/..." (what the user sees in a file manager).
std::string displayPath(const std::string& path) {
    if (path.rfind("sdmc:", 0) == 0) return path.substr(5);
    return path;
}

void showMessage(const std::string& text) {
    auto* dialog = new brls::Dialog(text);
    dialog->addButton("app/common/ok"_i18n, [] {});
    dialog->open();
}

// ── Local download page ────────────────────────────────────────────────

// The page the QR code opens on the phone: download the ZIP, then open the
// bugs topic in Telegram and attach it there.
std::string sharePage(const std::string& name) {
    return
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>" + "app/report/web_title"_i18n + "</title><style>"
        "body{font-family:sans-serif;background:#222;color:#fff;padding:20px;max-width:560px;margin:0 auto}"
        ".card{background:#333;padding:20px;border-radius:8px;line-height:1.5}"
        ".step{margin-top:18px}"
        "a.btn{display:block;text-align:center;padding:14px;margin-top:8px;border-radius:6px;"
        "color:#fff;text-decoration:none;font-size:17px}"
        ".dl{background:#007bff}.tg{background:#2aabee}"
        ".name{opacity:.7;font-size:14px;word-break:break-all}"
        "</style></head><body><h2>TorrentShopNX</h2><div class=\"card\">"
        "<p>" + "app/report/web_text"_i18n + "</p>"
        "<div class=\"step\">" + "app/report/web_step1"_i18n +
        "<a class=\"btn dl\" href=\"/" + name + "\" download>" + "app/report/web_download"_i18n + "</a>"
        "<div class=\"name\">" + name + "</div></div>"
        "<div class=\"step\">" + "app/report/web_step2"_i18n +
        "<a class=\"btn tg\" href=\"" + kBugsTopicUrl + "\">" + "app/report/web_open_topic"_i18n + "</a></div>"
        "</div></body></html>";
}

// Serves the report while the QR dialog is open; closing the dialog stops it.
class ShareServer {
public:
    static ShareServer& instance() {
        static ShareServer s;
        return s;
    }

    bool start(const std::string& zip_path) {
        stop();
        std::ifstream in(zip_path, std::ios::binary);
        if (!in.is_open()) return false;
        auto zip = std::make_shared<std::string>((std::istreambuf_iterator<char>(in)),
                                                 std::istreambuf_iterator<char>());
        const std::string name = zip_path.substr(zip_path.find_last_of('/') + 1);

        auto svr = std::make_unique<httplib::Server>();
        const std::string page = sharePage(name);
        svr->Get("/", [page](const httplib::Request&, httplib::Response& res) {
            res.set_content(page, "text/html; charset=utf-8");
        });
        svr->Get("/" + name, [zip, name](const httplib::Request&, httplib::Response& res) {
            res.set_header("Content-Disposition", "attachment; filename=\"" + name + "\"");
            res.set_content(*zip, "application/zip");
        });

        errno = 0;
        if (!svr->bind_to_port("0.0.0.0", kSharePort)) {
            const int err = errno;
            util::logLine("report: share server cannot bind port " + std::to_string(kSharePort) +
                          " errno=" + std::to_string(err) + " (" + std::strerror(err) + ")");
            return false;
        }
        std::lock_guard<std::mutex> lock(mtx_);
        svr_ = std::move(svr);
        httplib::Server* raw = svr_.get();
        thread_ = std::thread([raw]() { raw->listen_after_bind(); });
        util::logLine("report: share server started for " + name);
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!svr_) return;
        svr_->stop();
        if (thread_.joinable()) thread_.join();
        svr_.reset();
        util::logLine("report: share server stopped");
    }

    ~ShareServer() { stop(); }

private:
    std::mutex mtx_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread thread_;
};

void showShareQr(const std::string& zip_path) {
    const std::string ip = RemoteAddView::getLocalIpAddress();
#ifdef __SWITCH__
    // gethostid() is 0 -> "127.0.0.1" when the console has no network. (PC
    // test builds often resolve to loopback too; there the page still opens
    // in a browser on the same machine.)
    if (ip.empty() || ip == "127.0.0.1") {
        showMessage(brls::getStr("app/report/no_network", displayPath(zip_path)));
        return;
    }
#endif
    if (!ShareServer::instance().start(zip_path)) {
        showMessage(brls::getStr("app/report/share_failed", displayPath(zip_path)));
        return;
    }
    const std::string url = "http://" + ip + ":" + std::to_string(kSharePort) + "/";
    QrDialog::open("app/report/share_qr_title"_i18n, url, "app/report/share_qr_hint"_i18n,
                   [] { ShareServer::instance().stop(); });
}

// ── Building ──────────────────────────────────────────────────────────

struct Progress {
    brls::Dialog* dialog = nullptr;
    brls::Label* label = nullptr;
    // Set by Cancel: the dialog has closed (and freed) itself and must not
    // be touched any more.
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
};

Progress openProgress(const std::string& text) {
    Progress p;
    auto* box = new brls::Box(brls::Axis::COLUMN);
    box->setPadding(30, 40, 30, 40);
    box->setAlignItems(brls::AlignItems::CENTER);
    p.label = new brls::Label();
    p.label->setFontSize(18.0f);
    p.label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    p.label->setText(text);
    box->addView(p.label);

    p.dialog = new brls::Dialog(box);
    p.dialog->setCancelable(false);
    auto cancelled = p.cancelled;
    p.dialog->addButton("app/common/cancel"_i18n, [cancelled] { cancelled->store(true); });
    p.dialog->open();
    return p;
}

// Builds a report behind a progress dialog and hands it to `next`. With
// keep_dialog the dialog stays open for the next step (which then owns it
// and the busy flag); otherwise it is closed first. Nothing happens if the
// user cancelled.
void buildThen(std::function<void(const report::BuildResult&, Progress)> next, bool keep_dialog = false) {
    if (g_busy.exchange(true)) {
        brls::Application::notify("app/report/busy"_i18n);
        return;
    }
    Progress p = openProgress("app/report/building"_i18n);
    const std::string message = g_message;
    brls::async([p, message, next, keep_dialog]() {
        report::BuildResult built = report::buildReport(message);
        brls::sync([p, built, next, keep_dialog]() {
            if (p.cancelled->load()) {
                g_busy = false;
                return;
            }
            if (!built.ok) {
                g_busy = false;
                p.dialog->close([built] {
                    showMessage(brls::getStr("app/report/build_failed", built.error));
                });
                return;
            }
            if (keep_dialog) {
                next(built, p);
                return;
            }
            g_busy = false;
            p.dialog->close([built, next, p] { next(built, p); });
        });
    });
}

std::string uploadErrorText(const std::string& error) {
    if (error == "rate_limited") return "app/report/rate_limited"_i18n;
    return error;
}

void sendNow() {
    buildThen([](const report::BuildResult& built, Progress p) {
        p.label->setText("app/report/uploading"_i18n);
        const std::string zip_path = built.zip_path;
        const std::string message = g_message;
        brls::async([p, zip_path, message]() {
            report::UploadResult up = report::uploadReport(zip_path, message, p.cancelled.get());
            brls::sync([p, up, zip_path]() {
                g_busy = false;
                if (p.cancelled->load()) return;
                p.dialog->close([up, zip_path] {
                    if (up.ok) {
                        g_message.clear();
                        QrDialog::open(brls::getStr("app/report/sent_title", up.report_id), kBugsTopicUrl,
                                       brls::getStr("app/report/sent_hint", up.report_id));
                        return;
                    }
                    auto* dialog = new brls::Dialog(brls::getStr("app/report/send_failed", uploadErrorText(up.error)));
                    dialog->addButton("app/report/via_phone_btn"_i18n, [zip_path] { showShareQr(zip_path); });
                    dialog->addButton("app/common/cancel"_i18n, [] {});
                    dialog->open();
                });
            });
        });
    }, /*keep_dialog=*/true);
}

} // namespace

const std::string& message() { return g_message; }
void setMessage(const std::string& text) { g_message = text; }

void startSend() {
    if (!report::canUpload()) {
        startShare();
        return;
    }
    util::logLine("report: send requested, showing consent");
    const std::string contact = config::ConfigManager::instance().getReportContact();
    const std::string text = "app/report/consent"_i18n + "\n\n" +
        (contact.empty() ? "app/report/consent_no_contact"_i18n
                         : brls::getStr("app/report/consent_contact", contact));
    auto* dialog = new brls::Dialog(text);
    dialog->addButton("app/report/send_btn"_i18n, [] {
        util::logLine("report: sending confirmed by user");
        sendNow();
    });
    dialog->addButton("app/common/cancel"_i18n, [] { util::logLine("report: sending cancelled by user"); });
    dialog->open();
}

void startShare() {
    util::logLine("report: download via QR requested");
    buildThen([](const report::BuildResult& built, Progress) { showShareQr(built.zip_path); });
}

void startSave() {
    util::logLine("report: save to SD requested");
    buildThen([](const report::BuildResult& built, Progress) {
        showMessage(brls::getStr("app/report/saved", displayPath(built.zip_path)));
    });
}

} // namespace ui::bugreport
