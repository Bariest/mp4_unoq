/* mcu_display.cpp -- the MCU's half of the app's Display page (/v1/display).
 *
 * The ESP's http_display.cc did all of it on the robot. Here the GIF files are
 * on Linux (python/display_api.py: the list, previews, uploads, deletes, and
 * sending the chosen file into RAM first); the screen is here:
 *   GET  /v1/display/state  {"on","loop","id","flip180","mirror","shift_x","shift_y","brightness","eyes_alive","loaded"}
 *   POST /v1/display/set    the ESP's POST /v1/display body: the settings, then the animation
 * Same rules and messages as http_display.cc's apply_settings() and POST handler. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "shim_esp.h"
#include "cap_display.h"
#include "gif_store.h"
#include "mcu_display.h"

namespace {

/* a tiny reader for the flat JSON objects the app sends */
const char *find_key(const char *body, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return nullptr;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p != ':') return nullptr;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    return p;
}

bool get_bool(const char *body, const char *key, bool *out)
{
    const char *p = find_key(body, key);
    if (!p) return false;
    if (!strncmp(p, "true", 4)) { *out = true; return true; }
    if (!strncmp(p, "false", 5)) { *out = false; return true; }
    return false;
}

bool get_num(const char *body, const char *key, double *out)
{
    const char *p = find_key(body, key);
    if (!p || !(*p == '-' || (*p >= '0' && *p <= '9'))) return false;
    *out = strtod(p, nullptr);
    return true;
}

bool get_str(const char *body, const char *key, char *out, size_t n)
{
    const char *p = find_key(body, key);
    if (!p || *p != '"') return false;
    ++p;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) {
        if (*p == '\\' && p[1]) ++p;
        out[i++] = *p++;
    }
    out[i] = 0;
    return true;
}

const char *apply_settings(const char *body)
{
    bool f = false, m = false, bf, bm;
    const bool has_f = get_bool(body, "flip180", &bf), has_m = get_bool(body, "mirror", &bm);
    if (has_f || has_m) {
        cap_display_get_orientation(&f, &m);
        if (has_f) f = bf;
        if (has_m) m = bm;
        cap_display_set_orientation(f, m);
    }
    double sx, sy;
    const bool has_x = get_num(body, "shift_x", &sx), has_y = get_num(body, "shift_y", &sy);
    if (has_x || has_y) {
        int x = 0, y = 0;
        cap_display_get_shift(&x, &y);
        if (has_x) x = (int)sx;
        if (has_y) y = (int)sy;
        if (cap_display_set_shift(x, y) == ESP_ERR_INVALID_ARG) return "shift_x / shift_y: -40 to 40 pixels";
    }
    double br;
    if (get_num(body, "brightness", &br)) {
        if (br < 0 || br > 100) return "brightness: 0 to 100";
        cap_display_set_brightness((int)br);
    }
    bool alive;
    if (get_bool(body, "eyes_alive", &alive)) cap_display_set_eyes_alive(alive);
    char s[128];
    if (get_str(body, "emotion", s, sizeof s) && s[0]) {
        if (cap_display_show_emotion(s) == ESP_ERR_NOT_FOUND) return "emotion: not one the face knows";
    }
    if (get_str(body, "text", s, sizeof s)) {
        if (s[0]) cap_display_show_text(s);
        else cap_display_clear();
    }
    char bg[40], eyes[40];
    const bool has_bg = get_str(body, "background", bg, sizeof bg), has_eyes = get_str(body, "eyes", eyes, sizeof eyes);
    if (has_bg || has_eyes) {
        const int32_t bg_rgb = has_bg ? cap_display_parse_color(bg) : -1;
        const int32_t eye_rgb = has_eyes ? cap_display_parse_color(eyes) : -1;
        if ((has_bg && bg[0] && bg_rgb < 0) || (has_eyes && eyes[0] && eye_rgb < 0))
            return "colours are a name (black, white, red, green, blue, yellow, orange, "
                   "purple, pink, cyan, grey) or hex like #00ff00";
        if (bg_rgb >= 0 || eye_rgb >= 0) cap_display_set_colors(bg_rgb, eye_rgb);
    }
    return nullptr;
}

void state_json(std::string *out)
{
    cap_display_anim_state_t st;
    cap_display_anim_get(&st);
    bool flip = false, mirror = false;
    int sx = 0, sy = 0;
    cap_display_get_orientation(&flip, &mirror);
    cap_display_get_shift(&sx, &sy);
    char b[320];
    snprintf(b, sizeof b,
             "{\"on\":%s,\"loop\":%s,\"id\":\"%s\",\"flip180\":%s,\"mirror\":%s,\"shift_x\":%d,\"shift_y\":%d,"
             "\"brightness\":%d,\"eyes_alive\":%s,\"available\":%s,\"loaded\":\"%s\"}",
             st.on ? "true" : "false", st.loop ? "true" : "false", st.id, flip ? "true" : "false",
             mirror ? "true" : "false", sx, sy, cap_display_brightness(),
             cap_display_face_alive() ? "true" : "false", cap_display_available() ? "true" : "false",
             mpx_gif_loaded());
    *out = b;
}

}  // namespace

bool mcu_display_route(const char *method, const char *path, const char *body, int *status, std::string *out,
                       const char **ctype)
{
    if (strncmp(path, "/v1/display/", 12) != 0) return false;
    *ctype = "application/json";
    if (!strcmp(path, "/v1/display/state") && !strcmp(method, "GET")) {
        *status = 200;
        state_json(out);
        return true;
    }
    if (!strcmp(path, "/v1/display/set") && !strcmp(method, "POST")) {
        const char *bad = apply_settings(body ? body : "");
        if (bad) {
            *status = 400;
            *ctype = "text/plain";
            *out = bad;
            return true;
        }
        bool on = true, has_on = get_bool(body, "on", &on), loop = true;
        get_bool(body, "loop", &loop);
        char id[32] = "";
        get_str(body, "id", id, sizeof id);
        if (has_on || id[0]) {             /* a settings-only POST leaves the animation alone */
            if (!on) {
                cap_display_anim_stop();
            } else {
                if (!id[0]) {
                    cap_display_anim_state_t st;
                    cap_display_anim_get(&st);
                    strncpy(id, st.id[0] ? st.id : "blink", sizeof id - 1);
                }
                const esp_err_t err = cap_display_anim_play(id, loop);
                if (err != ESP_OK) {
                    *ctype = "text/plain";
                    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_ARG) { *status = 404; *out = "No such animation"; }
                    else if (err == ESP_ERR_NO_MEM) { *status = 500; *out = "Not enough memory to play it right now"; }
                    else if (err == ESP_ERR_INVALID_STATE) { *status = 503; *out = "No screen"; }
                    else { *status = 500; *out = "Could not play that GIF"; }
                    return true;
                }
            }
        }
        *status = 200;
        state_json(out);
        return true;
    }
    *status = 404;
    *ctype = "text/plain";
    *out = "not found";
    return true;
}
