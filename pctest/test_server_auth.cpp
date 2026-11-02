// TorrServer login (net::setServerAuth): the Basic auth header goes only to
// the TorrServer's scheme/host/port, and HttpClient sends it.
//
//   g++ -std=c++17 -Ipctest/compat -Iinclude -Isource pctest/test_server_auth.cpp
//       source/net/http_client.cpp source/utils/log.cpp -lcurl -lws2_32 -liphlpapi -lpthread
//
// With TSNX_AUTH_TEST_PORT=<port> it also sends real requests to a local server
// on that port that wants user "admin" / password "p@ss:word" (and to port+1,
// which must get no header). Exit code = number of failed checks.
#include "net/http_client.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

std::atomic<bool> g_appExiting{false};   // extern in utils/log.h

static int g_fail = 0;

static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

int main() {
    using net::authHeaderFor;
    using net::setServerAuth;

    check(authHeaderFor("http://192.168.1.5:8090/echo").empty(), "no login set -> no header");

    // "admin:p@ss:word" in base64
    const std::string want = "Authorization: Basic YWRtaW46cEBzczp3b3Jk";
    setServerAuth("192.168.1.5:8090", "admin", "p@ss:word");
    check(authHeaderFor("http://192.168.1.5:8090/echo") == want, "address without http:// matches");
    check(authHeaderFor("HTTP://192.168.1.5:8090/stream/x?link=1") == want, "scheme case ignored, path ignored");
    check(authHeaderFor("http://192.168.1.5:8091/echo").empty(), "other port -> no header");
    check(authHeaderFor("http://192.168.1.6:8090/echo").empty(), "other host -> no header");
    check(authHeaderFor("https://192.168.1.5:8090/echo").empty(), "other scheme -> no header");
    check(authHeaderFor("https://raw.githubusercontent.com/x.json").empty(), "catalog host -> no header");
    check(authHeaderFor("http://192.168.1.5.evil.com:8090/").empty(), "look-alike host -> no header");

    setServerAuth("http://ts.local/", "u", "p");
    check(authHeaderFor("http://ts.local:80/echo") == "Authorization: Basic dTpw", "default port 80 filled in");
    setServerAuth("http://ts.local/", "", "p");
    check(authHeaderFor("http://ts.local/echo").empty(), "empty login turns it off");

    if (const char* p = std::getenv("TSNX_AUTH_TEST_PORT")) {
        const std::string port = p;
        const std::string other = std::to_string(std::atoi(p) + 1);
        net::HttpClient http;
        http.setTimeout(3);
        setServerAuth("127.0.0.1:" + port, "", "");
        check(http.httpGet("http://127.0.0.1:" + port + "/echo").status_code == 401, "live: no login -> 401");
        setServerAuth("127.0.0.1:" + port, "admin", "wrong");
        check(http.httpGet("http://127.0.0.1:" + port + "/echo").status_code == 401, "live: wrong password -> 401");
        setServerAuth("127.0.0.1:" + port, "admin", "p@ss:word");
        check(http.httpGet("http://127.0.0.1:" + port + "/echo").status_code == 200, "live: GET with login -> 200");
        check(http.httpPost("http://127.0.0.1:" + port + "/torrents", "{\"action\":\"list\"}").status_code == 200,
              "live: POST with login -> 200");
        size_t got = 0;
        int code = http.httpGetStream("http://127.0.0.1:" + port + "/stream/a?link=1&index=0&play", 0, 4,
                                      [&](const void*, size_t n) { got += n; return n; });
        check((code == 200 || code == 206) && got > 0, "live: stream with login -> data");
        auto o = http.httpGet("http://127.0.0.1:" + other + "/echo");
        check(o.status_code == 200 && o.body == "no-auth", "live: other port gets no Authorization header");
    }

    std::printf("\n%d failed\n", g_fail);
    return g_fail;
}
