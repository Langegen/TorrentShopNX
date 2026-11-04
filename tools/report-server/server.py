#!/usr/bin/env python3
"""TorrentShopNX bug report server: receives report ZIPs from the app and
posts them to a private Telegram chat. Python 3.8+, standard library only.

POST /v1/report   body: ZIP built by the app (source/utils/bug_report.cpp)
  headers: X-TSNX-Version (required), X-TSNX-Firmware, X-TSNX-Lang,
           X-TSNX-Prev, X-TSNX-Contact (Telegram @username, percent-encoded),
           X-TSNX-Message (user's note, percent-encoded)
  reply:   {"ok": true, "id": "R-7F3A2C"}  or  {"ok": false, "error": "..."}

Every report is written to DATA_DIR and sent to Telegram. If Telegram is
unreachable it is retried every few minutes. Whatever happens, files older
than RETENTION_HOURS are deleted: reports never stay on the server longer.

Settings come from environment variables (see tsnx-reports.env.example).
"""

import json
import os
import re
import secrets
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

BOT_TOKEN = os.environ.get("TELEGRAM_BOT_TOKEN", "")
CHAT_ID = os.environ.get("TELEGRAM_CHAT_ID", "")
THREAD_ID = os.environ.get("TELEGRAM_THREAD_ID", "")
TELEGRAM_API = os.environ.get("TELEGRAM_API", "https://api.telegram.org").rstrip("/")
LISTEN_HOST = os.environ.get("LISTEN_HOST", "0.0.0.0")
LISTEN_PORT = int(os.environ.get("LISTEN_PORT", "8787"))
DATA_DIR = Path(os.environ.get("DATA_DIR", os.environ.get("STATE_DIRECTORY", "./reports")))
RETENTION_HOURS = float(os.environ.get("RETENTION_HOURS", "72"))
MAX_REPORT_BYTES = int(float(os.environ.get("MAX_REPORT_MB", "10")) * 1024 * 1024)
RATE_PER_IP = int(os.environ.get("RATE_PER_IP_PER_MINUTE", "3"))
RATE_GLOBAL = int(os.environ.get("RATE_GLOBAL_PER_MINUTE", "30"))
# Behind a reverse proxy (Caddy, nginx) the client address is in X-Forwarded-For.
TRUST_PROXY = os.environ.get("TRUST_PROXY", "0") == "1"

MAX_MESSAGE_CHARS = 600
# Cleanup + delivery retries run this often.
RETRY_EVERY_SECONDS = int(os.environ.get("HOUSEKEEPING_SECONDS", "300"))
ID_ALPHABET = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"  # no 0/O, 1/I
ID_RE = re.compile(r"^R-[%s]{6}$" % ID_ALPHABET)


def log(msg):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), msg, flush=True)


# ── Rate limiting ──────────────────────────────────────────────────────

class RateLimiter:
    """At most `limit` events per key in a sliding 60-second window."""

    def __init__(self, limit):
        self.limit = limit
        self.events = {}
        self.lock = threading.Lock()

    def allow(self, key):
        if self.limit <= 0:
            return True
        now = time.monotonic()
        with self.lock:
            recent = [t for t in self.events.get(key, []) if now - t < 60]
            if len(recent) >= self.limit:
                self.events[key] = recent
                return False
            recent.append(now)
            self.events[key] = recent
            if len(self.events) > 10000:  # forget idle addresses
                self.events = {k: v for k, v in self.events.items() if v and now - v[-1] < 60}
            return True


per_ip = RateLimiter(RATE_PER_IP)
global_limit = RateLimiter(RATE_GLOBAL)


# ── Report data ────────────────────────────────────────────────────────

def new_report_id():
    return "R-" + "".join(secrets.choice(ID_ALPHABET) for _ in range(6))


def decoded_header(headers, name):
    try:
        return urllib.parse.unquote(headers.get(name, ""), errors="strict")
    except UnicodeDecodeError:
        return ""


def short_header(headers, name, max_len=32):
    value = re.sub(r"[^\w.\- ]", "", headers.get(name, "").strip())[:max_len]
    return value or "?"


def contact(headers):
    value = decoded_header(headers, "X-TSNX-Contact").strip()
    return value if re.fullmatch(r"@[A-Za-z0-9_]{5,32}", value) else None


def user_message(headers):
    text = decoded_header(headers, "X-TSNX-Message")
    text = re.sub(r"[\x00-\x08\x0b-\x1f\x7f]", "", text).strip()
    return text[:MAX_MESSAGE_CHARS] + "…" if len(text) > MAX_MESSAGE_CHARS else text


def caption_for(report_id, headers, size):
    sender = contact(headers) or "(Telegram не указан)"
    message = user_message(headers)
    caption = (
        f"🐞 {report_id} от {sender}\n"
        f"TorrentShopNX {short_header(headers, 'X-TSNX-Version')}"
        f" · FW {short_header(headers, 'X-TSNX-Firmware')}"
        f" · {short_header(headers, 'X-TSNX-Lang', 8)}\n"
        f"Прошлый запуск: {short_header(headers, 'X-TSNX-Prev', 16)} · {size // 1024} КБ"
    )
    if message:
        caption += "\n\n" + message
    return caption[:1024]


def zip_path(report_id):
    return DATA_DIR / f"{report_id}.zip"


def meta_path(report_id):
    return DATA_DIR / f"{report_id}.json"


def write_json(path, data):
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
    os.replace(tmp, path)


# ── Telegram ───────────────────────────────────────────────────────────

def send_to_telegram(report_id, caption):
    """Posts the ZIP as a document. True when Telegram accepted it."""
    boundary = "----tsnx" + secrets.token_hex(12)
    fields = {"chat_id": CHAT_ID, "caption": caption}
    if THREAD_ID:
        fields["message_thread_id"] = THREAD_ID
    body = bytearray()
    for name, value in fields.items():
        body += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"\r\n\r\n"
                 f"{value}\r\n").encode("utf-8")
    body += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"document\"; "
             f"filename=\"{report_id}.zip\"\r\nContent-Type: application/zip\r\n\r\n").encode("utf-8")
    body += zip_path(report_id).read_bytes()
    body += f"\r\n--{boundary}--\r\n".encode("utf-8")

    req = urllib.request.Request(
        f"{TELEGRAM_API}/bot{BOT_TOKEN}/sendDocument",
        data=bytes(body),
        headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            return json.loads(resp.read().decode("utf-8")).get("ok", False)
    except urllib.error.HTTPError as e:
        log(f"telegram rejected {report_id}: {e.code} {e.read()[:300]!r}")
    except Exception as e:  # network errors: retried later
        log(f"telegram unreachable for {report_id}: {e}")
    return False


_in_flight = set()
_in_flight_lock = threading.Lock()


def deliver(report_id):
    # The request thread and the retry loop may both try; one send at a time.
    with _in_flight_lock:
        if report_id in _in_flight:
            return
        _in_flight.add(report_id)
    try:
        meta_file = meta_path(report_id)
        try:
            meta = json.loads(meta_file.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return
        if meta.get("delivered") or not zip_path(report_id).exists():
            return
        if send_to_telegram(report_id, meta["caption"]):
            meta["delivered"] = True
            write_json(meta_file, meta)
            log(f"{report_id} delivered to Telegram")
    finally:
        with _in_flight_lock:
            _in_flight.discard(report_id)


# ── Housekeeping ───────────────────────────────────────────────────────

def cleanup():
    """Deletes every report received more than RETENTION_HOURS ago,
    delivered or not."""
    cutoff = time.time() - RETENTION_HOURS * 3600
    removed = 0
    for path in DATA_DIR.glob("R-*"):
        report_id = path.name.split(".")[0]
        try:
            received = json.loads(meta_path(report_id).read_text(encoding="utf-8"))["received"]
        except (OSError, ValueError, KeyError):
            # No readable metadata (half-written report): judge by the file.
            try:
                received = path.stat().st_mtime
            except FileNotFoundError:
                continue
        if received < cutoff:
            try:
                path.unlink()
                removed += 1
            except FileNotFoundError:
                pass
    if removed:
        log(f"cleanup: removed {removed} files older than {RETENTION_HOURS:g} h")


def housekeeping_loop():
    while True:
        try:
            cleanup()
            for meta_file in DATA_DIR.glob("R-*.json"):
                report_id = meta_file.stem
                if ID_RE.match(report_id) and zip_path(report_id).exists():
                    deliver(report_id)
        except Exception as e:
            log(f"housekeeping failed: {e}")
        time.sleep(RETRY_EVERY_SECONDS)


# ── HTTP ───────────────────────────────────────────────────────────────

class Handler(BaseHTTPRequestHandler):
    server_version = "tsnx-reports"
    sys_version = ""

    def log_message(self, fmt, *args):  # no per-request access log (IPs)
        pass

    def reply(self, status, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def client_ip(self):
        if TRUST_PROXY:
            forwarded = self.headers.get("X-Forwarded-For", "")
            if forwarded:
                return forwarded.split(",")[0].strip()
        return self.client_address[0]

    def do_GET(self):
        if self.path in ("/", "/health"):
            body = b"TorrentShopNX report server\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.reply(404, {"ok": False, "error": "not_found"})

    def do_POST(self):
        if self.path != "/v1/report":
            return self.reply(404, {"ok": False, "error": "not_found"})
        if not BOT_TOKEN or not CHAT_ID:
            return self.reply(500, {"ok": False, "error": "server_not_configured"})
        # Only the app sends this header; it keeps random scanners out.
        if not self.headers.get("X-TSNX-Version"):
            return self.reply(400, {"ok": False, "error": "bad_request"})
        if not per_ip.allow(self.client_ip()) or not global_limit.allow("all"):
            return self.reply(429, {"ok": False, "error": "rate_limited"})

        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            return self.reply(411, {"ok": False, "error": "length_required"})
        if length <= 0 or length > MAX_REPORT_BYTES:
            return self.reply(413, {"ok": False, "error": "too_large"})
        data = self.rfile.read(length)
        if len(data) != length:
            return self.reply(400, {"ok": False, "error": "bad_request"})
        if not data.startswith(b"PK\x03\x04"):  # ZIP local file header
            return self.reply(400, {"ok": False, "error": "not_a_zip"})

        report_id = new_report_id()
        while zip_path(report_id).exists():
            report_id = new_report_id()
        zip_path(report_id).write_bytes(data)
        write_json(meta_path(report_id), {
            "caption": caption_for(report_id, self.headers, len(data)),
            "received": int(time.time()),
            "delivered": False,
        })
        log(f"{report_id} received, {len(data)} bytes, from {contact(self.headers) or 'anonymous'}")
        # Answer at once even if Telegram is slow or down: the report is on
        # disk and the housekeeping loop retries the delivery.
        self.reply(200, {"ok": True, "id": report_id})
        deliver(report_id)


def main():
    if not BOT_TOKEN or not CHAT_ID:
        log("warning: TELEGRAM_BOT_TOKEN / TELEGRAM_CHAT_ID are not set, reports are refused")
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    threading.Thread(target=housekeeping_loop, daemon=True).start()
    server = ThreadingHTTPServer((LISTEN_HOST, LISTEN_PORT), Handler)
    server.daemon_threads = True
    log(f"listening on {LISTEN_HOST}:{LISTEN_PORT}, data in {DATA_DIR}, "
        f"kept for {RETENTION_HOURS:g} h")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
