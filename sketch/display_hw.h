/* display_hw.h -- the LCD of the UNO Q carrier board (ST7789 320x240 on the J1
 * FPC) under LVGL: the same calls as the ESP build's display/display_hw.h, so
 * cap_display.c, cap_display_face.c and cap_display_orient.c run unchanged.
 * Pins in board_config.h; how it works in display_hw.cpp. */
#ifndef DISPLAY_HW_H
#define DISPLAY_HW_H

#include <stdbool.h>
#include <stdint.h>
#include "shim_esp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* the esp_lcd calls cap_display_orient.c makes, on this panel */
typedef struct mpx_panel *esp_lcd_panel_handle_t;
esp_err_t esp_lcd_panel_mirror(esp_lcd_panel_handle_t panel, bool mirror_x, bool mirror_y);
esp_err_t esp_lcd_panel_swap_xy(esp_lcd_panel_handle_t panel, bool swap);
esp_err_t esp_lcd_panel_set_gap(esp_lcd_panel_handle_t panel, int x_gap, int y_gap);
esp_err_t esp_lcd_panel_invert_color(esp_lcd_panel_handle_t panel, bool invert);
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t panel, bool on);

esp_err_t display_hw_init(void);
bool      display_hw_available(void);
esp_err_t display_hw_lock(void);
void      display_hw_unlock(void);
esp_lcd_panel_handle_t display_hw_panel(void);
void      display_hw_base_mirror(bool *mirror_x, bool *mirror_y);
esp_err_t display_hw_backlight(int percent);

/* UNO Q additions */
/* Straight to the panel, past LVGL (the GIF player): RGB565 as LVGL keeps it.
 * Caller holds the lock. */
void display_hw_draw(int x, int y, int w, int h, const uint16_t *px);
void display_hw_fill(int x, int y, int w, int h, uint16_t rgb565);
/* The screen from power-on until the face: title, a status line under it. */
void display_hw_boot_status(const char *line);
void display_hw_boot_done(void);
uint32_t display_hw_spi_hz(void);
uint32_t display_hw_frames(void);

#ifdef __cplusplus
}
#endif

#endif
