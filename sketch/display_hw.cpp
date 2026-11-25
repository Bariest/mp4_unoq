/* display_hw.cpp -- the carrier board's LCD: ST7789, 320x240, on SPI1 of the
 * STM32U585 (A1 = PA5 SCK, A3 = PA7 MOSI, A0 = PA4 chip select), DC on D5,
 * RESET on D4, backlight on D6 through a P-FET (low = lit).
 *
 * SPI1 is not in the UNO Q's devicetree (A0..A3 are analog inputs there), so
 * it is driven here by its registers, as spi_port.cpp does for the servo bus's
 * pins: the clock from PCLK2 / 4 (40 MHz), mode 2 as the ESP drove this panel,
 * transmit only, chip select by hand. Pixels go as 16-bit frames: LVGL's
 * little-endian RGB565 leaves MSB first, which is the panel's order, so no
 * byte swapping (the ESP needed swap_bytes for its 8-bit SPI).
 *
 * The rest is what esp_lvgl_port did on the ESP: one LVGL display with a
 * partial draw buffer, a thread running lv_timer_handler() under a lock
 * (display_hw_lock), and the panel's ST_MADCTL for the base rotation (swap XY +
 * mirror X: landscape, as the ESP set it) and for cap_display_orient.c's flips.
 *
 * From power-on until Linux has given the settings, a boot screen shows what
 * the robot is waiting for (display_hw_boot_status). */
#include <Arduino.h>
#include <string.h>
#include "board_config.h"
#include "display_hw.h"
#include "src/lvgl/lvgl.h"

#if defined(__ZEPHYR__)
#include <zephyr/kernel.h>
#endif

#define BUF_LINES   30
#define LOCK_MS     1000

struct mpx_panel { uint8_t madctl; int gap_x, gap_y; };

namespace {

const char *TAG = "display_hw";

mpx_panel     s_panel = { 0, 0, 0 };
lv_display_t *s_disp;
bool          s_ready;
SemaphoreHandle_t s_lock;
uint32_t      s_hz = 0;
uint32_t      s_frames = 0;
lv_obj_t     *s_boot_screen, *s_boot_line, *s_boot_time;
lv_timer_t   *s_boot_timer;
int           s_boot_updates;     /* status lines set so far: 1 = still waiting for Linux */
char          s_boot_text[200] = "starting";
volatile bool s_boot_dirty;

/* ST7789 */
constexpr uint8_t ST_CASET = 0x2A, ST_RASET = 0x2B, ST_RAMWR = 0x2C, ST_MADCTL = 0x36, ST_COLMOD = 0x3A,
                  ST_SLPOUT = 0x11, ST_INVON = 0x21, ST_INVOFF = 0x20, ST_DISPON = 0x29, ST_DISPOFF = 0x28, ST_SWRESET = 0x01;
constexpr uint8_t MAD_MY = 0x80, MAD_MX = 0x40, MAD_MV = 0x20;
/* Base orientation of this robot's LCD: turned 180 degrees from the first build
 * (that was MIRROR_X = true, MIRROR_Y = false, which reads upside down on this
 * robot). The boot screen and the face both start from it; the app's Display
 * page "turn 180" still flips the face from here. */
constexpr bool BASE_SWAP_XY = true, BASE_MIRROR_X = false, BASE_MIRROR_Y = true, PANEL_INVERT = true;

/* ── SPI1 by its registers ─────────────────────────────────────────────── */
#if defined(ARDUINO_UNO_Q)
constexpr uintptr_t LCD_SPI1_ADDR = 0x40013000UL, LCD_GPIOA_ADDR = 0x42020000UL, LCD_RCC_ADDR = 0x46020C00UL;
inline volatile uint32_t &reg(uintptr_t a) { return *reinterpret_cast<volatile uint32_t *>(a); }
#define LCD_CR1  reg(LCD_SPI1_ADDR + 0x00)
#define LCD_CR2  reg(LCD_SPI1_ADDR + 0x04)
#define LCD_CFG1 reg(LCD_SPI1_ADDR + 0x08)
#define LCD_CFG2 reg(LCD_SPI1_ADDR + 0x0C)
#define LCD_SR   reg(LCD_SPI1_ADDR + 0x14)
#define LCD_IFCR reg(LCD_SPI1_ADDR + 0x18)
constexpr uintptr_t LCD_TXDR = LCD_SPI1_ADDR + 0x20;
constexpr uint32_t LCD_SR_TXP = 1u << 1, LCD_SR_TXC = 1u << 12, LCD_CR1_SPE = 1u << 0, LCD_CR1_CSTART = 1u << 9, LCD_CR1_SSI = 1u << 12;

uint32_t pclk2_hz()
{
#ifdef CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC
    uint32_t hclk = CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC;
#else
    uint32_t hclk = 160000000UL;
#endif
    const uint32_t ppre2 = (reg(LCD_RCC_ADDR + 0x20) >> 8) & 7u;     /* RCC_CFGR2.PPRE2: 0xx = /1, 100 = /2 ... */
    return ppre2 < 4 ? hclk : hclk >> (ppre2 - 3);
}

void spi_init(uint32_t want_hz)
{
    reg(LCD_RCC_ADDR + 0x8C) |= 1u << 0;                /* AHB2ENR1.GPIOAEN */
    reg(LCD_RCC_ADDR + 0xA4) |= 1u << 12;               /* APB2ENR.SPI1EN */
    (void)reg(LCD_RCC_ADDR + 0xA4);
    /* PA5 = SCK, PA7 = MOSI: alternate function 5, very high speed */
    volatile uint32_t &moder = reg(LCD_GPIOA_ADDR + 0x00), &ospeedr = reg(LCD_GPIOA_ADDR + 0x08), &afrl = reg(LCD_GPIOA_ADDR + 0x20);
    for (int pin : { 5, 7 }) {
        afrl = (afrl & ~(0xFu << (4 * pin))) | (5u << (4 * pin));
        ospeedr |= 3u << (2 * pin);
        moder = (moder & ~(3u << (2 * pin))) | (2u << (2 * pin));
    }
    LCD_CR1 = LCD_CR1_SSI;                              /* disabled; internal NSS high (no mode fault) */
    const uint32_t pclk = pclk2_hz();
    uint32_t mbr = 0;                               /* clock = pclk / 2^(mbr+1) */
    while (mbr < 7 && (pclk >> (mbr + 1)) > want_hz) ++mbr;
    s_hz = pclk >> (mbr + 1);
    LCD_CFG1 = (mbr << 28) | 7u;                    /* 8-bit frames */
    /* master, software NSS, CPOL 1 CPHA 0 (mode 2), transmit only, keep the pins driven while off */
    LCD_CFG2 = (1u << 31) | (1u << 26) | (1u << 25) | (1u << 22) | (1u << 17);
}

inline void spi_start(bool sixteen)
{
    LCD_CR1 = LCD_CR1_SSI;
    LCD_CFG1 = (LCD_CFG1 & ~0x1FFu) | (sixteen ? (15u | (1u << 5)) : 7u);   /* 16-bit frames: packets of 2 */
    LCD_CR2 = 0;
    LCD_CR1 = LCD_CR1_SSI | LCD_CR1_SPE;
    LCD_CR1 = LCD_CR1_SSI | LCD_CR1_SPE | LCD_CR1_CSTART;
}

inline void spi_end()
{
    while (!(LCD_SR & LCD_SR_TXC)) {}
    LCD_IFCR = 0xFF8u;
    LCD_CR1 = LCD_CR1_SSI;
}

void spi_bytes(const uint8_t *b, size_t n)
{
    spi_start(false);
    for (size_t i = 0; i < n; ++i) {
        while (!(LCD_SR & LCD_SR_TXP)) {}
        *reinterpret_cast<volatile uint8_t *>(LCD_TXDR) = b[i];
    }
    spi_end();
}

void spi_pixels(const uint16_t *px, size_t n, bool repeat)
{
    spi_start(true);
    size_t i = 0;
    if (repeat) {
        const uint32_t two = (uint32_t)px[0] | ((uint32_t)px[0] << 16);
        for (; i + 1 < n; i += 2) {
            while (!(LCD_SR & LCD_SR_TXP)) {}
            reg(LCD_TXDR) = two;                    /* two 16-bit frames, low half first */
        }
        if (i < n) {
            while (!(LCD_SR & LCD_SR_TXP)) {}
            *reinterpret_cast<volatile uint16_t *>(LCD_TXDR) = px[0];
        }
    } else {
        if (n && (reinterpret_cast<uintptr_t>(px) & 2u)) {   /* to a word boundary */
            while (!(LCD_SR & LCD_SR_TXP)) {}
            *reinterpret_cast<volatile uint16_t *>(LCD_TXDR) = px[i++];
        }
        for (; i + 1 < n; i += 2) {
            while (!(LCD_SR & LCD_SR_TXP)) {}
            reg(LCD_TXDR) = *reinterpret_cast<const uint32_t *>(px + i);
        }
        if (i < n) {
            while (!(LCD_SR & LCD_SR_TXP)) {}
            *reinterpret_cast<volatile uint16_t *>(LCD_TXDR) = px[i];
        }
    }
    spi_end();
}
#else   /* host test build: no panel */
void spi_init(uint32_t want_hz) { s_hz = want_hz; }
void spi_bytes(const uint8_t *, size_t) {}
void spi_pixels(const uint16_t *, size_t, bool) {}
#endif

/* ── ST7789 ────────────────────────────────────────────────────────────── */
void cmd(uint8_t c, const uint8_t *data = nullptr, size_t n = 0)
{
    digitalWrite(A0, LOW);
    digitalWrite(LCD_DC_PIN, LOW);
    spi_bytes(&c, 1);
    if (n) {
        digitalWrite(LCD_DC_PIN, HIGH);
        spi_bytes(data, n);
    }
    digitalWrite(A0, HIGH);
}

void set_window(int x1, int y1, int x2, int y2)
{
    x1 += s_panel.gap_x; x2 += s_panel.gap_x;
    y1 += s_panel.gap_y; y2 += s_panel.gap_y;
    const uint8_t c[4] = { (uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2 };
    const uint8_t r[4] = { (uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2 };
    cmd(ST_CASET, c, 4);
    cmd(ST_RASET, r, 4);
}

#if !defined(ARDUINO_UNO_Q)
uint16_t s_host_fb[LCD_H_RES * LCD_V_RES];      /* the host test build's "panel" (tools/hosttest) */
#endif

void write_rect(int x, int y, int w, int h, const uint16_t *px, bool repeat)
{
    if (w <= 0 || h <= 0) return;
#if !defined(ARDUINO_UNO_Q)
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c)
            if (y + r >= 0 && y + r < LCD_V_RES && x + c >= 0 && x + c < LCD_H_RES)
                s_host_fb[(y + r) * LCD_H_RES + x + c] = repeat ? px[0] : px[r * w + c];
#endif
    set_window(x, y, x + w - 1, y + h - 1);
    digitalWrite(A0, LOW);
    digitalWrite(LCD_DC_PIN, LOW);
    const uint8_t c = ST_RAMWR;
    spi_bytes(&c, 1);
    digitalWrite(LCD_DC_PIN, HIGH);
    spi_pixels(px, (size_t)w * h, repeat);
    digitalWrite(A0, HIGH);
    ++s_frames;
}

void panel_init()
{
    pinMode(A0, OUTPUT);
    digitalWrite(A0, HIGH);
    pinMode(LCD_DC_PIN, OUTPUT);
    pinMode(LCD_RST_PIN, OUTPUT);
    spi_init(LCD_SPI_HZ);
    digitalWrite(LCD_RST_PIN, HIGH);
    delay(5);
    digitalWrite(LCD_RST_PIN, LOW);
    delay(20);
    digitalWrite(LCD_RST_PIN, HIGH);
    delay(120);
    cmd(ST_SWRESET);
    delay(150);
    cmd(ST_SLPOUT);
    delay(120);
    const uint8_t rgb565 = 0x55;
    cmd(ST_COLMOD, &rgb565, 1);
    s_panel.madctl = (BASE_SWAP_XY ? MAD_MV : 0) | (BASE_MIRROR_X ? MAD_MX : 0) | (BASE_MIRROR_Y ? MAD_MY : 0);
    cmd(ST_MADCTL, &s_panel.madctl, 1);
    cmd(PANEL_INVERT ? ST_INVON : ST_INVOFF);
    const uint16_t black = 0;
    write_rect(0, 0, LCD_H_RES, LCD_V_RES, &black, true);   /* no random RAM when the light comes on */
    cmd(ST_DISPON);
    delay(10);
}

/* ── LVGL ──────────────────────────────────────────────────────────────── */
uint32_t tick_ms() { return millis(); }

void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    write_rect(a->x1, a->y1, a->x2 - a->x1 + 1, a->y2 - a->y1 + 1, reinterpret_cast<const uint16_t *>(px), false);
    lv_display_flush_ready(d);
}

void boot_screen_update()
{
    if (!s_boot_line || !s_boot_dirty) return;
    s_boot_dirty = false;
    lv_label_set_text(s_boot_line, s_boot_text);
}

void lvgl_task(void *)
{
    for (;;) {
        uint32_t wait = 33;
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            boot_screen_update();
            wait = lv_timer_handler();
            xSemaphoreGive(s_lock);
        }
        if (wait < 5) wait = 5;
        if (wait > 33) wait = 33;
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

/* once a second: how long it has been, and after 100 s without Linux a hint */
void boot_timer_cb(lv_timer_t *)
{
    if (!s_boot_time) return;
    const uint32_t s = millis() / 1000;
    if (s_boot_updates <= 1 && s >= 100) {
        lv_label_set_text_fmt(s_boot_time, "%lu s - no answer from Linux yet. If this stays, the router "
                              "is not at 460800 baud (README, step 1)", (unsigned long)s);
    } else {
        lv_label_set_text_fmt(s_boot_time, "%lu s", (unsigned long)s);
    }
}

void boot_screen_build()
{
    s_boot_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_boot_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_boot_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_boot_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(s_boot_screen);
#if LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
#endif
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFD000), LV_PART_MAIN);
    lv_label_set_text(title, "Mini Pupper");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -30);
    lv_obj_t *sub = lv_label_create(s_boot_screen);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x9AA0A8), LV_PART_MAIN);
    lv_label_set_text(sub, "Arduino UNO Q");
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 4);
    s_boot_line = lv_label_create(s_boot_screen);
    lv_obj_set_width(s_boot_line, LCD_H_RES - 20);
    lv_obj_set_style_text_align(s_boot_line, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_boot_line, lv_color_hex(0xF0F0F0), LV_PART_MAIN);
    lv_label_set_long_mode(s_boot_line, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_boot_line, s_boot_text);
    lv_obj_align(s_boot_line, LV_ALIGN_BOTTOM_MID, 0, -40);
    s_boot_time = lv_label_create(s_boot_screen);
    lv_obj_set_width(s_boot_time, LCD_H_RES - 20);
    lv_obj_set_style_text_align(s_boot_time, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_boot_time, lv_color_hex(0x9AA0A8), LV_PART_MAIN);
    lv_label_set_long_mode(s_boot_time, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_boot_time, "");
    lv_obj_align(s_boot_time, LV_ALIGN_BOTTOM_MID, 0, -6);
    s_boot_timer = lv_timer_create(boot_timer_cb, 1000, nullptr);
    lv_screen_load(s_boot_screen);
}

}  // namespace

extern "C" {

esp_err_t display_hw_init(void)
{
#if !LCD_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_ready) return ESP_OK;
    pinMode(LCD_BL_PIN, OUTPUT);
    digitalWrite(LCD_BL_PIN, LCD_BL_ACTIVE_LOW ? HIGH : LOW);       /* dark until there is a picture */
    panel_init();
    s_lock = xSemaphoreCreateMutex();
    lv_init();
    lv_tick_set_cb(tick_ms);
    s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    const uint32_t bytes = LCD_H_RES * BUF_LINES * 2;
    void *buf = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_DMA);
    if (!s_disp || !buf) {
        ESP_LOGE(TAG, "no memory for LVGL");
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, buf, nullptr, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    boot_screen_build();
    lv_refr_now(s_disp);
    s_ready = true;
    display_hw_backlight(80);
    /* below the gait (4), the Bridge (5) and the IMU (6): ESP priority 2 -> Zephyr 8 */
    if (xTaskCreate(lvgl_task, "lvgl", 8192, nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "no LVGL task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ST7789 %dx%d on SPI1 at %lu kHz (mode 2), LVGL %d.%d.%d, draw buffer %d lines",
             LCD_H_RES, LCD_V_RES, (unsigned long)(s_hz / 1000), LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
             LVGL_VERSION_PATCH, BUF_LINES);
    return ESP_OK;
#endif
}

bool display_hw_available(void) { return s_ready; }

esp_err_t display_hw_lock(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(LOCK_MS)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void display_hw_unlock(void)
{
    if (s_ready) xSemaphoreGive(s_lock);
}

esp_lcd_panel_handle_t display_hw_panel(void) { return s_ready ? &s_panel : nullptr; }

void display_hw_base_mirror(bool *mx, bool *my)
{
    if (mx) *mx = BASE_MIRROR_X;
    if (my) *my = BASE_MIRROR_Y;
}

esp_err_t display_hw_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    const int duty = percent * 255 / 100;
    /* D6 through the P-FET: low = lit */
    if (percent >= 100) digitalWrite(LCD_BL_PIN, LCD_BL_ACTIVE_LOW ? LOW : HIGH);
    else if (percent <= 0) digitalWrite(LCD_BL_PIN, LCD_BL_ACTIVE_LOW ? HIGH : LOW);
    else analogWrite(LCD_BL_PIN, LCD_BL_ACTIVE_LOW ? 255 - duty : duty);
    return ESP_OK;
}

esp_err_t esp_lcd_panel_mirror(esp_lcd_panel_handle_t p, bool mx, bool my)
{
    if (!p) return ESP_ERR_INVALID_ARG;
    p->madctl = (p->madctl & ~(MAD_MX | MAD_MY)) | (mx ? MAD_MX : 0) | (my ? MAD_MY : 0);
    cmd(ST_MADCTL, &p->madctl, 1);
    return ESP_OK;
}

esp_err_t esp_lcd_panel_swap_xy(esp_lcd_panel_handle_t p, bool swap)
{
    if (!p) return ESP_ERR_INVALID_ARG;
    p->madctl = (p->madctl & ~MAD_MV) | (swap ? MAD_MV : 0);
    cmd(ST_MADCTL, &p->madctl, 1);
    return ESP_OK;
}

esp_err_t esp_lcd_panel_set_gap(esp_lcd_panel_handle_t p, int x, int y)
{
    if (!p) return ESP_ERR_INVALID_ARG;
    p->gap_x = x;
    p->gap_y = y;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_invert_color(esp_lcd_panel_handle_t p, bool inv)
{
    if (!p) return ESP_ERR_INVALID_ARG;
    cmd(inv ? ST_INVON : ST_INVOFF);
    return ESP_OK;
}

esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t p, bool on)
{
    if (!p) return ESP_ERR_INVALID_ARG;
    cmd(on ? ST_DISPON : ST_DISPOFF);
    return ESP_OK;
}

void display_hw_draw(int x, int y, int w, int h, const uint16_t *px)
{
    if (s_ready) write_rect(x, y, w, h, px, false);
}

void display_hw_fill(int x, int y, int w, int h, uint16_t c)
{
    if (s_ready) write_rect(x, y, w, h, &c, true);
}

void display_hw_boot_status(const char *line)
{
    mpx_critical_enter();
    strncpy(s_boot_text, line ? line : "", sizeof s_boot_text - 1);
    s_boot_text[sizeof s_boot_text - 1] = 0;
    s_boot_dirty = true;
    ++s_boot_updates;
    mpx_critical_exit();
}

void display_hw_boot_done(void)
{
    if (display_hw_lock() != ESP_OK) return;
    if (s_boot_screen && lv_screen_active() != s_boot_screen) {
        if (s_boot_timer) lv_timer_delete(s_boot_timer);
        s_boot_timer = nullptr;
        lv_obj_delete(s_boot_screen);
        s_boot_screen = s_boot_line = s_boot_time = nullptr;
    }
    display_hw_unlock();
}

uint32_t display_hw_spi_hz(void) { return s_hz; }
#if !defined(ARDUINO_UNO_Q)
const uint16_t *display_hw_host_fb(void) { return s_host_fb; }
#endif
uint32_t display_hw_frames(void) { return s_frames; }

}  // extern "C"
