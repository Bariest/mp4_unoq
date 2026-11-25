/* mcu_http.cpp -- the ESP's robot and Servo Studio HTTP routes, answered on the
 * MCU and carried over the Bridge.
 *
 * mcu_http_routes_inc.h is http_server.cc's own handler code (copied by
 * tools/port_from_esp.py), so every JSON reply is the ESP's, byte for byte. This
 * file gives it what esp_http_server.h gave it on the ESP - a request it can
 * read and a response it can write - and a route table. The Linux side
 * (python/esp_api.py) forwards GET/POST /v1/robot/... and /v1/studio/... here
 * through the Bridge (sketch.ino: mp_http + mp_http_more, the reply in chunks
 * because a Bridge message is 256 bytes).
 *
 * What the routes reach that this robot does not have:
 *   - skills (WASM movement skills)       none: by_gait() finds nothing
 *   - the Raspberry Pi link               always "standalone"
 *   - deep sleep (power action "sleep")   the servos rest instead; the UNO Q keeps running
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "shim_esp.h"
#include "robot.h"
#include "driver_board.h"
#include "battery.h"
#include "mcu_display.h"
#include "mpx_policy.h"
#include "cJSON.h"
#include "mcu_http.h"

namespace mcu_http {
namespace {

const char *TAG = "http";

/* ── esp_http_server.h, the part the handlers use ──────────────────────── */
struct httpd_req_t {
    const char *method;
    std::string query;
    const char *body;
    size_t body_len;
    size_t body_pos;
    int status;
    std::string ctype;
    std::string out;
    int content_len;
};
#define HTTPD_RESP_USE_STRLEN -1
#define HTTPD_SOCK_ERR_TIMEOUT -3
typedef enum {
    HTTPD_400_BAD_REQUEST = 400, HTTPD_404_NOT_FOUND = 404, HTTPD_405_METHOD_NOT_ALLOWED = 405,
    HTTPD_408_REQ_TIMEOUT = 408, HTTPD_500_INTERNAL_SERVER_ERROR = 500
} httpd_err_code_t;

int httpd_req_recv(httpd_req_t *r, char *buf, size_t len)
{
    const size_t n = r->body_len - r->body_pos < len ? r->body_len - r->body_pos : len;
    memcpy(buf, r->body + r->body_pos, n);
    r->body_pos += n;
    return static_cast<int>(n);
}

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status) { r->status = atoi(status); return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t) { r->ctype = t; return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *) { return ESP_OK; }

esp_err_t httpd_resp_send(httpd_req_t *r, const char *s, ssize_t len)
{
    if (len < 0) len = static_cast<ssize_t>(strlen(s));
    r->out.append(s, static_cast<size_t>(len));
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *s, ssize_t len)
{
    if (!s) return ESP_OK;                             /* the end of a chunked reply */
    return httpd_resp_send(r, s, len);
}
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s) { return httpd_resp_send(r, s, -1); }
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *r, const char *s) { return s ? httpd_resp_send(r, s, -1) : ESP_OK; }
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t code, const char *msg)
{
    r->status = static_cast<int>(code);
    r->ctype = "text/plain";
    r->out = msg ? msg : "";
    return ESP_OK;
}
esp_err_t httpd_resp_send_500(httpd_req_t *r) { return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal Server Error"); }

esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t len)
{
    if (r->query.empty()) return ESP_ERR_NOT_FOUND;
    if (r->query.size() + 1 > len) return ESP_ERR_INVALID_SIZE;
    memcpy(buf, r->query.c_str(), r->query.size() + 1);
    return ESP_OK;
}

esp_err_t httpd_query_key_value(const char *q, const char *key, char *val, size_t len)
{
    const size_t kl = strlen(key);
    const char *p = q;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        if (static_cast<size_t>(end - p) > kl && strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t o = 0;
            while (v < end && o + 1 < len) {          /* %xx and + decoded */
                if (*v == '%' && end - v >= 3) {
                    char hex[3] = { v[1], v[2], 0 };
                    val[o++] = static_cast<char>(strtol(hex, nullptr, 16));
                    v += 3;
                } else {
                    val[o++] = (*v == '+') ? ' ' : *v;
                    ++v;
                }
            }
            val[o] = 0;
            return ESP_OK;
        }
        p = amp ? amp + 1 : nullptr;
    }
    return ESP_ERR_NOT_FOUND;
}

/* ── what the routes mention that this robot does not have ─────────────── */
namespace skills {
struct Entry { std::string file, slug, provides_gait; };
const Entry *by_gait(const char *) { return nullptr; }
const std::vector<Entry> &all() { static const std::vector<Entry> none; return none; }
}
const char *start_skill_async(const std::string &, const std::string &, const char *) { return "409 Conflict"; }
namespace pi_link { inline const char *state_str() { return "standalone"; } }
bool mpx_sleep_start()
{
    /* No deep sleep for the UNO Q from here: lie down and switch the servos off,
     * which is what the ESP does before it sleeps. */
    robot::rest_now();
    return true;
}

/* the ESP's Wi-Fi, which the RL page switches off for a run "without Wi-Fi": not here (Linux has the network) */
namespace network {
enum class StaState { Disconnected, Connecting, Connected };
inline std::string wifi_sta_get_ssid() { return std::string(); }
inline StaState wifi_sta_get_state() { return StaState::Connected; }
}
#define WIFI_PS_NONE 0
inline esp_err_t esp_wifi_stop() { return ESP_ERR_NOT_SUPPORTED; }
inline esp_err_t esp_wifi_start() { return ESP_ERR_NOT_SUPPORTED; }
inline esp_err_t esp_wifi_connect() { return ESP_ERR_NOT_SUPPORTED; }
inline esp_err_t esp_wifi_disconnect() { return ESP_ERR_NOT_SUPPORTED; }
inline esp_err_t esp_wifi_set_ps(int) { return ESP_ERR_NOT_SUPPORTED; }

}  // namespace

#include "mcu_http_routes_inc.h"   /* .h, not .inc: the App Lab build only copies known extensions */
#include "mcu_http_policy_inc.h"   /* the RL page (http_policy.cc) */

namespace {

struct Route { const char *method, *path; esp_err_t (*fn)(httpd_req_t *); };

const Route ROUTES[] = {
    MPX_ROUTES_ROBOT           /* mcu_http_routes_inc.h: as http_server.cc registers them */
    MPX_ROUTES_POLICY          /* mcu_http_policy_inc.h: as http_policy.cc registers them */
};

}  // namespace

Response handle(const char *method, const char *uri, const char *body, size_t body_len)
{
    httpd_req_t r;
    r.method = method;
    r.body = body ? body : "";
    r.body_len = body ? body_len : 0;
    r.body_pos = 0;
    r.content_len = static_cast<int>(r.body_len);
    r.status = 200;
    r.ctype = "text/plain";
    std::string path(uri);
    const size_t q = path.find('?');
    if (q != std::string::npos) {
        r.query = path.substr(q + 1);
        path.resize(q);
    }
    {
        int st = 200;
        std::string out;
        const char *ct = "application/json";
        if (mcu_display_route(method, path.c_str(), r.body, &st, &out, &ct)) return Response{ st, ct, out };
    }
    for (const Route &rt : ROUTES) {
        if (path == rt.path && strcmp(method, rt.method) == 0) {
            rt.fn(&r);
            return Response{ r.status, r.ctype, r.out };
        }
    }
    for (const Route &rt : ROUTES) {
        if (path == rt.path) return Response{ 405, "text/plain", "method not allowed" };
    }
    return Response{ 404, "text/plain", "not found" };
}

}  // namespace mcu_http
