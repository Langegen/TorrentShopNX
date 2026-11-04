// TorrentShopNX bug report server (Cloudflare Worker).
//
// POST /v1/report   body: ZIP built by the app (source/utils/bug_report.cpp)
//   headers: X-TSNX-Version, X-TSNX-Firmware, X-TSNX-Lang, X-TSNX-Prev,
//            X-TSNX-Contact (the user's Telegram @username, percent-encoded),
//            X-TSNX-Message (percent-encoded user note)
//   reply:   {"ok": true, "id": "R-7F3A2C"}  or  {"ok": false, "error": "..."}
//
// The ZIP goes to a private Telegram chat as a document. The caption carries
// the report id and the sender's @username, so you can write to them, and a
// user who posts "R-7F3A2C" in the bugs topic can be matched to the archive.
//
// Secrets (wrangler secret put ...): TELEGRAM_BOT_TOKEN, TELEGRAM_CHAT_ID.
// Optional var TELEGRAM_THREAD_ID: topic id when the chat is a forum group.

const MAX_REPORT_BYTES = 10 * 1024 * 1024;
const MAX_MESSAGE_CHARS = 600;
const ID_ALPHABET = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"; // no 0/O, 1/I

function json(status, obj) {
  return new Response(JSON.stringify(obj), {
    status,
    headers: { "Content-Type": "application/json; charset=utf-8" },
  });
}

function reportId() {
  const bytes = new Uint8Array(6);
  crypto.getRandomValues(bytes);
  let id = "R-";
  for (const b of bytes) id += ID_ALPHABET[b % ID_ALPHABET.length];
  return id;
}

function decoded(request, name) {
  try {
    return decodeURIComponent(request.headers.get(name) || "");
  } catch {
    return "";
  }
}

// Short header values only: they end up in the Telegram caption.
function header(request, name, maxLen = 32) {
  const v = (request.headers.get(name) || "").trim();
  return v.replace(/[^\w.\- ]/g, "").slice(0, maxLen) || "?";
}

// "@name" if it is a valid Telegram username, otherwise null.
function contact(request) {
  const v = decoded(request, "X-TSNX-Contact").trim();
  return /^@[A-Za-z0-9_]{5,32}$/.test(v) ? v : null;
}

function userMessage(request) {
  const text = decoded(request, "X-TSNX-Message")
    .replace(/[\u0000-\u0008\u000B-\u001F\u007F]/g, "")
    .trim();
  return text.length > MAX_MESSAGE_CHARS ? text.slice(0, MAX_MESSAGE_CHARS) + "…" : text;
}

async function sendToTelegram(env, id, zip, caption) {
  const form = new FormData();
  form.append("chat_id", env.TELEGRAM_CHAT_ID);
  if (env.TELEGRAM_THREAD_ID) form.append("message_thread_id", env.TELEGRAM_THREAD_ID);
  form.append("caption", caption.slice(0, 1024));
  form.append("document", new Blob([zip], { type: "application/zip" }), `${id}.zip`);

  const resp = await fetch(`https://api.telegram.org/bot${env.TELEGRAM_BOT_TOKEN}/sendDocument`, {
    method: "POST",
    body: form,
  });
  if (resp.ok) return true;
  console.log(`telegram sendDocument failed: ${resp.status} ${await resp.text()}`);
  return false;
}

async function handleReport(request, env) {
  if (!env.TELEGRAM_BOT_TOKEN || !env.TELEGRAM_CHAT_ID) {
    return json(500, { ok: false, error: "server_not_configured" });
  }
  // Only the app sends this header; it keeps random scanners out.
  if (!request.headers.get("X-TSNX-Version")) {
    return json(400, { ok: false, error: "bad_request" });
  }

  const ip = request.headers.get("CF-Connecting-IP") || "unknown";
  if (env.PER_IP_LIMITER) {
    const { success } = await env.PER_IP_LIMITER.limit({ key: ip });
    if (!success) return json(429, { ok: false, error: "rate_limited" });
  }
  if (env.GLOBAL_LIMITER) {
    const { success } = await env.GLOBAL_LIMITER.limit({ key: "all" });
    if (!success) return json(429, { ok: false, error: "rate_limited" });
  }

  const declared = Number(request.headers.get("Content-Length") || 0);
  if (declared > MAX_REPORT_BYTES) return json(413, { ok: false, error: "too_large" });
  const zip = new Uint8Array(await request.arrayBuffer());
  if (zip.length === 0 || zip.length > MAX_REPORT_BYTES) {
    return json(413, { ok: false, error: "too_large" });
  }
  // ZIP local file header "PK\x03\x04".
  if (zip[0] !== 0x50 || zip[1] !== 0x4b || zip[2] !== 0x03 || zip[3] !== 0x04) {
    return json(400, { ok: false, error: "not_a_zip" });
  }

  const id = reportId();
  const from = contact(request);
  const message = userMessage(request);
  const country = (request.cf && request.cf.country) || "?";

  const caption =
    `🐞 ${id} от ${from || "(Telegram не указан)"}\n` +
    `TorrentShopNX ${header(request, "X-TSNX-Version")} · FW ${header(request, "X-TSNX-Firmware")}` +
    ` · ${header(request, "X-TSNX-Lang", 8)} · ${country}\n` +
    `Прошлый запуск: ${header(request, "X-TSNX-Prev", 16)} · ${(zip.length / 1024).toFixed(0)} КБ` +
    (message ? `\n\n${message}` : "");

  const delivered = await sendToTelegram(env, id, zip, caption);
  if (!delivered) return json(502, { ok: false, error: "delivery_failed" });
  return json(200, { ok: true, id });
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/v1/report") {
      if (request.method !== "POST") return json(405, { ok: false, error: "method_not_allowed" });
      try {
        return await handleReport(request, env);
      } catch (e) {
        console.log(`report failed: ${e && e.stack ? e.stack : e}`);
        return json(500, { ok: false, error: "internal_error" });
      }
    }
    if (url.pathname === "/" || url.pathname === "/health") {
      return new Response("TorrentShopNX report server\n", { status: 200 });
    }
    return json(404, { ok: false, error: "not_found" });
  },
};
