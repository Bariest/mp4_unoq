/* mpx_unoq -- Mini Pupper (MP4 servo boards) on the Arduino UNO Q, running the
 * ESP32-S3 firmware's own movement code (mpv2onlyRL).
 *
 * The STM32U585 does what the ESP's robot task did, with the same code:
 *   robot.cpp / moves_mpe.inc   every gait and move, the 200 Hz servo frames,
 *                               power / rest, look, joystick, calibration offsets
 *   driver_board.c              the AT32 protocol, Servo Studio's parameter access
 *   mcu_http.cpp                the ESP's /v1/robot and /v1/studio HTTP handlers
 * (all copied from mpv2onlyRL by tools/port_from_esp.py; see shim_esp.h for how the
 * ESP-IDF calls map onto this board).
 *
 * Linux (python/) serves the ESP's web app (pwa-claw) and forwards those routes
 * here over the Bridge. It also keeps what the ESP kept in NVS (calibration,
 * config, current caps) and streams the TrajOpt / Character move rows, which do
 * not fit in this chip's sketch space.
 *
 * Boot: the servos stay limp until Linux has sent the saved settings (mp_nvs ...
 * mp_nvs_done), so the robot never stands up on default calibration. Then
 * robot::init() runs as on the ESP: boards found, range read, soft start to the
 * stand.
 */
/* The ESP's HTTP handlers run on the Bridge's thread here: give it the stack an
 * ESP httpd task has (the Servo Studio dump builds a ~900-byte reply). */
#define UPDATE_THREAD_STACK_SIZE 12288
#include <Arduino_RouterBridge.h>
#include <stdio.h>
#include <string.h>

#include "shim_esp.h"
#include "mpx_glue.h"
#include "driver_board.h"
#include "mcu_http.h"
#include "spi_port.h"
#include "tj_stream.h"
#include "display_hw.h"
#include "gif_store.h"
#include "net_status.h"

#define MPX_FW_VERSION 5       /* 5 = firmware-dev: carrier board, face, IMU, battery (4 = mpv2onlyRL moves) */

/* The Bridge to Linux. 460800, not the core's 115200: the face's GIFs and the
 * RL policy's observations need the room. The router on Linux must run at the
 * same speed (README: "sudo systemctl edit arduino-router"); until it does,
 * the boot screen says so. */
#define MPX_BRIDGE_BAUD 460800
#define HTTP_CHUNK 180         /* Bridge messages are 256 bytes */

static volatile bool g_nvs_done = false;
static volatile bool g_started = false;
static uint32_t g_http_count = 0;
static std::string g_resp;     /* the reply being fetched in chunks (one request at a time) */

using Bin = MsgPack::bin_t<uint8_t>;

/* ── Bridge functions (Bridge.provide: they run on the Bridge's own thread) ── */

/* registers of the Bridge's serial link, for the boot screen and mp_hello:
 * LPUART1 ISR and BRR, the RX pin level (PG8), RCC CR / BDCR / CCIPR3, reset flags */
static uint32_t rd32(uint32_t a) { return *reinterpret_cast<volatile uint32_t *>(a); }
static void link_diag(char *b, size_t n)
{
    snprintf(b, n, "isr %lx brr %lx rx %lu cr %lx bd %lx c3 %lx csr %lx",
             (unsigned long)rd32(0x46002400UL + 0x1C), (unsigned long)rd32(0x46002400UL + 0x0C),
             (unsigned long)((rd32(0x42021800UL + 0x10) >> 8) & 1u),
             (unsigned long)rd32(0x46020C00UL), (unsigned long)rd32(0x46020C00UL + 0xF0),
             (unsigned long)rd32(0x46020C00UL + 0xE8), (unsigned long)(rd32(0x46020C00UL + 0xF4) >> 24));
}

String mp_hello()
{
    char fmt[16];
    snprintf(fmt, sizeof fmt, "%.1f", 1.5);          /* does printf do floats? (the routes need it) */
    char b[320];
    snprintf(b, sizeof b,
             "fw=%d;started=%d;nvs=%d;scale=%d;probed=%d;boards=%d;spi_khz=%lu;edge=%d;mosi=%lu;frames=%lu;"
             "http=%lu;fmt=%s;mode=%s;imu=%d;lcd=%d;lcd_khz=%lu;lcd_frames=%lu;gif=%s;heap=%lu",
             MPX_FW_VERSION, g_started ? 1 : 0, g_nvs_done ? 1 : 0,
             (int)driver_board_get_scale_dd(), driver_board_probe_answered() ? 1 : 0,
             g_started ? (int)(driver_board_present_fresh(1) + driver_board_present_fresh(4) +
                               driver_board_present_fresh(7) + driver_board_present_fresh(10)) : 0,
             (unsigned long)(mpx_spi_hz() / 1000), mpx_spi_edge(), (unsigned long)mpx_spi_mosi_state(),
             (unsigned long)mpx_spi_frames(), (unsigned long)g_http_count, fmt,
             g_started ? mpx_robot_mode() : "-", g_started && mpx_imu_ok() ? 1 : 0,
             display_hw_available() ? 1 : 0, (unsigned long)(display_hw_spi_hz() / 1000),
             (unsigned long)display_hw_frames(), mpx_gif_loaded(),
             (unsigned long)heap_caps_get_largest_free_block(0));
    return String(b);
}

/* method, uri (path?query), body -> "status|content-type|total|inline"
 * inline = the whole reply when it is short and plain ASCII; otherwise empty and
 * Linux fetches the bytes with mp_http_more (binary: a chunk boundary may fall
 * inside a UTF-8 character, which a Bridge string would reject). */
String mp_http(String method, String uri, String body)
{
    ++g_http_count;
    int status = 503;
    std::string ctype = "application/json";
    if (!g_started) {
        g_resp = "{\"ok\":false,\"error\":\"robot starting\"}";
    } else {
        mcu_http::Response r = mcu_http::handle(method.c_str(), uri.c_str(), body.c_str(), body.length());
        status = r.status;
        ctype = r.ctype;
        g_resp.swap(r.body);
    }
    bool ascii = g_resp.size() <= HTTP_CHUNK;
    for (size_t i = 0; ascii && i < g_resp.size(); ++i) ascii = (unsigned char)g_resp[i] < 0x80;
    String out = String(status) + "|" + ctype.c_str() + "|" + String((unsigned long)g_resp.size()) + "|";
    if (ascii) out += g_resp.c_str();
    return out;
}

Bin mp_http_more(int offset)
{
    Bin b;
    if (offset < 0 || (size_t)offset >= g_resp.size()) return b;
    const size_t end = (size_t)offset + HTTP_CHUNK < g_resp.size() ? (size_t)offset + HTTP_CHUNK : g_resp.size();
    for (size_t i = (size_t)offset; i < end; ++i) b.push_back((uint8_t)g_resp[i]);
    return b;
}

/* the saved settings, one key at a time (type "i" int32, "b" blob), then done */
bool mp_nvs(String key, String type, Bin v)
{
    static bool first = true;
    if (first) { first = false; display_hw_boot_status("Loading the saved settings"); }
    if (type.length() != 1) return false;
    return mpx_nvs_load(key.c_str(), (uint8_t)type[0], v.data(), v.size());
}

bool mp_nvs_done()
{
    g_nvs_done = true;
    return true;
}

int mp_tj_push(int table, int start, Bin rows)
{
    return tj_stream_push(table, start, rows.data(), (int)rows.size());
}

/* the face's GIF, sent into RAM before it plays (python/display_api.py) */
bool mp_gif_begin(String id, int size) { return mpx_gif_begin(id.c_str(), (uint32_t)size); }
bool mp_gif_put(int offset, Bin data) { return mpx_gif_put((uint32_t)offset, data.data(), (uint32_t)data.size()); }
bool mp_gif_end(String id) { return mpx_gif_end(id.c_str()); }

/* the line under the eyes: the address to type in a browser */
bool mp_net(bool connected, String ip, String ssid)
{
    mpx_net_set(connected, ip.c_str(), ssid.c_str());
    if (!g_nvs_done) {
        String line = "Linux is up: " + ip + " - waiting for the robot app";
        display_hw_boot_status(line.c_str());
    }
    return true;
}

/* servo-bus tuning (edge 0..3 or -1, kHz or 0) */
String mp_spi(int edge, int khz)
{
    mpx_spi_set(edge, khz > 0 ? (uint32_t)khz * 1000UL : 0UL);
    return mp_hello();
}

/* Wait until the router on Linux answers, before Bridge.begin().
 *
 * With "Startup mode: Immediate" this sketch runs before Linux has booted.
 * Bridge.begin() writes its flush line and one "$/reset", then waits for the
 * answer with no timeout - a message sent before the router has opened the port
 * is lost, so after a power-on begin() waited forever ("Linux is starting" until
 * the sketch was flashed again, when the same two writes worked at once).
 *
 * So here exactly those two writes are repeated once a second until the router
 * answers: the flush line (printable bytes, which complete and discard any
 * message the router caught half of) and "$/reset" in MessagePack. Each round
 * also re-applies the port settings and clears the LPUART's error flags, in case
 * the line noise of the Linux boot left the receiver stuck. The boot screen shows
 * the rounds and the bytes heard, so a stall says which side is silent.
 *
 * The port is ARDUINO_ROUTER_SERIAL (the core's name for the Bridge's port): in
 * zephyr core 1.1.0 that is Serial2 = LPUART1, while Serial1 is USART1 on D0/D1.
 *
 * VDDIO2: the Bridge's LPUART1 is on PG7 (TX) / PG8 (RX), and PG2..PG15 run from
 * the separate VDDIO2 supply. If that supply comes up after the MCU (from the
 * Linux side, at power-on), the pins can stay isolated although zephyr set
 * IO2SV at its start - a flash later, with Linux up, finds them fine. So the
 * VDDIO2 monitor is switched on and, once it reports the supply ready, IO2SV is
 * set again and the pins re-applied (ARDUINO_ROUTER_SERIAL.begin). Shown as "io2" on screen. */
static void wait_for_router()
{
    static const char flush_line[] = "MCU starting RPC Bridge communication";
    static const uint8_t ask[] = { 0x94, 0x00, 0x7E,                    /* [0, 126, */
                                   0xA7, '$', '/', 'r', 'e', 's', 'e', 't',
                                   0x90 };                              /*  "$/reset", []] */
    volatile uint32_t *const lpuart1_icr = reinterpret_cast<volatile uint32_t *>(0x46002400UL + 0x20);
    volatile uint32_t *const pwr_svmcr = reinterpret_cast<volatile uint32_t *>(0x46020800UL + 0x10);
    volatile uint32_t *const pwr_svmsr = reinterpret_cast<volatile uint32_t *>(0x46020800UL + 0x3C);
    const uint32_t IO2VMEN = 1u << 25, IO2SV = 1u << 29, VDDIO2RDY = 1u << 25;
    bool io2_redone = false;
    uint32_t rounds = 0, heard = 0;
    for (;;) {
        *pwr_svmcr |= IO2VMEN;                      /* VDDIO2 voltage monitor on */
        if (!io2_redone && (*pwr_svmsr & VDDIO2RDY)) {
            *pwr_svmcr &= ~IO2SV;                   /* supply is there: validate it anew */
            delayMicroseconds(10);
            *pwr_svmcr |= IO2SV;
            io2_redone = true;
        }
        ARDUINO_ROUTER_SERIAL.begin(MPX_BRIDGE_BAUD);
        *lpuart1_icr = 0x0Fu;                       /* PECF FECF NECF ORECF */
        while (ARDUINO_ROUTER_SERIAL.available()) { ARDUINO_ROUTER_SERIAL.read(); ++heard; }
        ARDUINO_ROUTER_SERIAL.write(reinterpret_cast<const uint8_t *>(flush_line), sizeof flush_line - 1);
        ARDUINO_ROUTER_SERIAL.write(ask, sizeof ask);
        ++rounds;
        /* any answer starts [1, id, ...: 0x94 0x01 */
        uint8_t prev = 0;
        const uint32_t t0 = millis();
        while (millis() - t0 < 1000) {
            if (!ARDUINO_ROUTER_SERIAL.available()) { delay(2); continue; }
            const uint8_t b = (uint8_t)ARDUINO_ROUTER_SERIAL.read();
            ++heard;
            const bool answer = (prev == 0x94 && b == 0x01);
            prev = b;
            if (answer) {
                delay(50);                          /* the rest of the answer */
                while (ARDUINO_ROUTER_SERIAL.available()) ARDUINO_ROUTER_SERIAL.read();
                return;
            }
        }
        char d[96], line[200];
        link_diag(d, sizeof d);
        snprintf(line, sizeof line, "Linux is starting (asked %lu times, %lu bytes back, io2 %d%d%d)\n%s",
                 (unsigned long)rounds, (unsigned long)heard,
                 (*pwr_svmcr & IO2SV) ? 1 : 0, (*pwr_svmsr & VDDIO2RDY) ? 1 : 0, io2_redone ? 1 : 0, d);
        display_hw_boot_status(line);

        /* A flash (a warm reset with Linux already up) always finds the link
         * working. So if half a minute after a power-on nothing has come back,
         * reset the MCU once the same way. The reset-flags register says whether
         * that was already tried (SFTRSTF stays set until the next power-on). */
        const uint32_t SFTRSTF = 1u << 28;
        if (rounds >= 30 && heard == 0 && !(rd32(0x46020C00UL + 0xF4) & SFTRSTF)) {
            display_hw_boot_status("No answer from Linux: restarting the MCU once");
            delay(1500);
            __asm volatile("dsb");
            *reinterpret_cast<volatile uint32_t *>(0xE000ED0CUL) = 0x05FA0004UL;   /* AIRCR SYSRESETREQ */
            for (;;) { }
        }
    }
}

void setup()
{
    /* The screen first: it shows the boot while Linux starts (~1 min). */
    display_hw_init();
    display_hw_boot_status("Linux is starting");
    wait_for_router();                 /* returns once the router on Linux answers */
    Bridge.begin(MPX_BRIDGE_BAUD);
    display_hw_boot_status("Linux is up - waiting for the robot app");
    Bridge.provide("mp_hello", mp_hello);
    Bridge.provide("mp_http", mp_http);
    Bridge.provide("mp_http_more", mp_http_more);
    Bridge.provide("mp_nvs", mp_nvs);
    Bridge.provide("mp_nvs_done", mp_nvs_done);
    Bridge.provide("mp_tj_push", mp_tj_push);
    Bridge.provide("mp_spi", mp_spi);
    Bridge.provide("mp_gif_begin", mp_gif_begin);
    Bridge.provide("mp_gif_put", mp_gif_put);
    Bridge.provide("mp_gif_end", mp_gif_end);
    Bridge.provide("mp_net", mp_net);
}

void loop()
{
    /* start the robot once Linux has given us the saved settings */
    if (g_nvs_done && !g_started) {
        char d[96];                    /* the serial link's registers, working state (app log) */
        link_diag(d, sizeof d);
        Bridge.notify("mp_log", String("link: ") + d);
        mpx_glue_start();              /* robot, battery, face (mpx_glue.cpp) */
        g_started = true;
    }

    /* settings the robot changed (calibration, config, current caps): Linux saves them */
    char key[32];
    uint8_t type, v[40];
    size_t n;
    for (int i = 0; i < 4 && mpx_nvs_take_dirty(key, sizeof key, &type, v, &n); ++i) {
        const char t[2] = { (char)type, 0 };
        Bin b;
        for (size_t i = 0; i < n; ++i) b.push_back(v[i]);
        Bridge.notify("mp_nvs_save", String(key), String(t), b);
    }

    /* log lines */
    char line[160];
    for (int i = 0; i < 6 && mpx_log_take(line, sizeof line); ++i) Bridge.notify("mp_log", String(line));

    /* the face wants a GIF that is not in RAM (a looping one chosen before this boot) */
    char gif[32];
    if (mpx_gif_take_want(gif, sizeof gif)) Bridge.notify("mp_gif_want", String(gif));

    /* a TrajOpt / Character move is playing: ask Linux for its next rows */
    static uint32_t last_want = 0;
    int table, next, space;
    if (tj_stream_wanted(&table, &next, &space) && millis() - last_want >= 15) {
        last_want = millis();
        Bridge.notify("mp_tj_want", table, next, space);
    }

    delay(5);
}
