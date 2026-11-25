/* ── UNO Q: the player (tools/patches/face_anim_unoq.c, put here by
 * port_from_esp.py in place of the ESP's; the rest of this section is the ESP's).
 *
 * The GIF files stay on Linux (python/display_api.py). The one to play is sent
 * into this chip's RAM first (gif_store.cpp), decoded from there by LVGL's
 * AnimatedGIF, and drawn straight to the panel a line at a time
 * (display_hw_draw): no 150 KB frame canvas, which this chip could not spare
 * next to the file. The GIF rules are the ESP player's: a transparent pixel
 * keeps what is on the panel, and the previous frame's disposal runs before
 * the next frame (2 clears its area to the backdrop; 3, "put back what was
 * under it", does the same -- the ESP falls back to that too when it cannot
 * keep a copy). LVGL draws only the backdrop layer, once; nothing else on the
 * face redraws while an animation owns the screen. */

#define ANIM_ID_MAX     32

static lv_obj_t *s_anim_layer;               /* full-screen backdrop */

typedef struct {
    GIFIMAGE       gif;                      /* decoder state, ~25 KB */
    lv_timer_t    *timer;
    uint16_t       backdrop;                 /* RGB565 used to clear (disposal 2/3) */
    uint16_t       corner;                   /* the first frame's top-left pixel (probe) */
    int            ox, oy;                   /* where the GIF's canvas sits on the panel */
    int            fx, fy, fw, fh, fdisp;    /* the frame on screen, disposed before the next */
    bool           probe;                    /* decoding without drawing */
    bool           loop;
    bool           ending;                   /* play-once: last frame up, stop next tick */
    uint16_t       line[LCD_H_RES];
} anim_player_t;
static anim_player_t *s_pl;
static volatile bool s_anim_on;
static bool      s_anim_loop = true;
static char      s_anim_id[ANIM_ID_MAX];
static char      s_anim_dir[96] = "/fs/display";
static char      s_anim_pending[ANIM_ID_MAX]; /* restored at boot, started by the LVGL task */
static bool      s_anim_pending_loop;

static void anim_save(bool on, const char *id, bool loop)
{
    nvs_handle_t h;

    if (nvs_open("display", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, "anim_on", on ? 1 : 0);
    nvs_set_u8(h, "anim_loop", loop ? 1 : 0);
    if (id) {
        nvs_set_str(h, "anim_id", id);
    }
    nvs_commit(h);
    nvs_close(h);
}

/* One decoded line: runs of opaque pixels straight to the panel. */
static void anim_draw_cb(GIFDRAW *d)
{
    anim_player_t *pl = (anim_player_t *)d->pUser;
    const uint16_t *pal = d->pPalette;
    const uint8_t *src = d->pPixels;
    const int yy = d->iY + d->y + pl->oy;
    int x0 = d->iX + pl->ox, w = d->iWidth;

    if (pl->probe) {
        if (d->iY + d->y == 0 && d->iX == 0 && (!d->ucHasTransparency || src[0] != d->ucTransparent)) {
            pl->corner = pal[src[0]];
        }
        return;
    }
    if (yy < 0 || yy >= LCD_V_RES) {
        return;
    }
    if (x0 < 0) {
        src -= x0;
        w += x0;
        x0 = 0;
    }
    if (x0 + w > LCD_H_RES) {
        w = LCD_H_RES - x0;
    }
    if (w <= 0) {
        return;
    }
    if (!d->ucHasTransparency) {
        for (int i = 0; i < w; i++) {
            pl->line[i] = pal[src[i]];
        }
        display_hw_draw(x0, yy, w, 1, pl->line);
        return;
    }
    const uint8_t t = d->ucTransparent;
    for (int i = 0; i < w;) {
        while (i < w && src[i] == t) {
            i++;
        }
        const int start = i;
        while (i < w && src[i] != t) {
            pl->line[i - start] = pal[src[i]];
            i++;
        }
        if (i > start) {
            display_hw_draw(x0 + start, yy, i - start, 1, pl->line);
        }
    }
}

/* Decode the next frame. Returns >0 if more frames follow, 0 after the last
 * one (the next call starts from the top again). */
static int anim_next_frame(anim_player_t *pl, int *delay_ms)
{
    if (pl->fdisp == 2 || pl->fdisp == 3) {
        int x = pl->fx + pl->ox, y = pl->fy + pl->oy, w = pl->fw, h = pl->fh;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > LCD_H_RES) { w = LCD_H_RES - x; }
        if (y + h > LCD_V_RES) { h = LCD_V_RES - y; }
        if (w > 0 && h > 0) {
            display_hw_fill(x, y, w, h, pl->backdrop);
        }
    }
    *delay_ms = 0;
    const int more = GIF_playFrame(&pl->gif, delay_ms, pl);
    pl->fx = pl->gif.iX;
    pl->fy = pl->gif.iY;
    pl->fw = pl->gif.iWidth;
    pl->fh = pl->gif.iHeight;
    pl->fdisp = (pl->gif.ucGIFBits & 0x1c) >> 2;
    if (*delay_ms < 20) {
        *delay_ms = 100;                   /* browsers treat 0-10 ms as 100 ms too */
    }
    return more;
}

/* Caller holds the LVGL lock. Safe from inside the player's own timer. */
static void anim_player_free(void)
{
    anim_player_t *pl = s_pl;

    if (!pl) {
        return;
    }
    s_pl = NULL;
    if (pl->timer) {
        lv_timer_delete(pl->timer);
    }
    GIF_close(&pl->gif);
    heap_caps_free(pl);
}

static void anim_finish_once(void);

static void anim_timer_cb(lv_timer_t *t)
{
    anim_player_t *pl = (anim_player_t *)lv_timer_get_user_data(t);
    int ms = 100;

    if (pl->ending) {
        anim_finish_once();                /* frees pl and this timer */
        return;
    }
    /* Play-once: the last frame stays up for its own delay, then the eyes. */
    const int more = anim_next_frame(pl, &ms);
    if (more < 0) {
        ESP_LOGW(TAG, "animation '%s': decode error %d", s_anim_id, GIF_getLastError(&pl->gif));
        pl->ending = true;
    } else if (more == 0 && !pl->loop) {
        pl->ending = true;
    }
    lv_timer_set_period(t, (uint32_t)ms);
}

/* Caller holds the LVGL lock. */
static void anim_close_locked(void)
{
    anim_player_free();
    if (s_anim_layer) {
        lv_obj_delete(s_anim_layer);       /* LVGL redraws the face over the GIF */
        s_anim_layer = NULL;
    }
    s_anim_on = false;
    s_last_us = 0;
    if (s_status) {
        lv_obj_remove_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    }
}

static void anim_finish_once(void)
{
    anim_close_locked();
    ESP_LOGI(TAG, "animation '%s' finished -- back to the eyes", s_anim_id);
}

static esp_err_t anim_play_locked(const char *id, bool loop);

/* From face_tick_cb, LVGL lock held. True while an animation owns the screen. */
static bool anim_tick(void)
{
    if (s_anim_pending[0]) {
        /* The boot-time restore: the file comes from Linux first. */
        if (!mpx_gif_get(s_anim_pending, NULL)) {
            mpx_gif_want(s_anim_pending);
        } else {
            char id[ANIM_ID_MAX];
            strlcpy(id, s_anim_pending, sizeof(id));
            s_anim_pending[0] = '\0';
            if (anim_play_locked(id, s_anim_pending_loop) == ESP_OK) {
                ESP_LOGI(TAG, "animation '%s' restored", id);
            }
        }
    }
    return s_anim_on && s_pl;
}

/* Linux is about to replace the file in RAM (gif_store.cpp): let go of it. */
void cap_display_anim_release_file(void)
{
    if (display_hw_lock() != ESP_OK) {
        return;
    }
    if (s_pl) {
        anim_close_locked();
    }
    display_hw_unlock();
}

static esp_err_t face_build_locked(void);

/* Caller holds the LVGL lock. */
static esp_err_t anim_play_locked(const char *id, bool loop)
{
    uint32_t size = 0;
    uint8_t *data;
    int ms = 100, cw, ch, more;

    if (face_build_locked() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    anim_close_locked();
    data = mpx_gif_get(id, &size);
    if (!data) {
        ESP_LOGW(TAG, "animation: %s.gif is not loaded from Linux", id);
        mpx_gif_want(id);
        return ESP_ERR_NOT_FOUND;
    }
    if (size < 13 || memcmp(data, "GIF8", 4) != 0) {
        ESP_LOGW(TAG, "animation: %s.gif is not a GIF", id);
        return ESP_ERR_NOT_FOUND;
    }

    anim_player_t *pl = heap_caps_calloc(1, sizeof(*pl), MALLOC_CAP_SPIRAM);
    if (!pl) {
        return ESP_ERR_NO_MEM;
    }
    s_pl = pl;
    pl->loop = loop;
    GIF_begin(&pl->gif, GIF_PALETTE_RGB565_LE);
    if (!GIF_openRAM(&pl->gif, data, (int)size, anim_draw_cb)) {
        ESP_LOGW(TAG, "animation: %s.gif did not decode", id);
        anim_close_locked();
        return ESP_FAIL;
    }
    pl->gif.ucDrawType = GIF_DRAW_RAW;
    cw = GIF_getCanvasWidth(&pl->gif);
    ch = GIF_getCanvasHeight(&pl->gif);
    if (cw <= 0 || ch <= 0 || cw > MAX_WIDTH) {
        ESP_LOGW(TAG, "animation: %s.gif is %dx%d, cannot show it", id, cw, ch);
        anim_close_locked();
        return ESP_ERR_INVALID_SIZE;
    }
    pl->ox = (LCD_H_RES - cw) / 2;
    pl->oy = (LCD_V_RES - ch) / 2;
    const uint16_t gif_bg = pl->gif.pPalette[pl->gif.ucBackground];

    /* The first frame's corner is the picture's real background (the ESP
     * player reads it off its canvas): find it with a dry run of frame one. */
    pl->corner = gif_bg;
    pl->probe = true;
    (void)GIF_playFrame(&pl->gif, &ms, pl);
    pl->probe = false;
    GIF_reset(&pl->gif);
    pl->backdrop = pl->corner;

    /* The backdrop, drawn by LVGL once; no IP line over the animation. */
    if (s_status) {
        lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    }
    s_anim_layer = lv_obj_create(s_screen);
    if (!s_anim_layer) {
        anim_close_locked();
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_anim_layer);
    lv_obj_set_size(s_anim_layer, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_anim_layer,
                              lv_color_make((uint8_t)(((pl->backdrop >> 11) & 0x1F) << 3),
                                            (uint8_t)(((pl->backdrop >> 5) & 0x3F) << 2),
                                            (uint8_t)((pl->backdrop & 0x1F) << 3)),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_anim_layer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_anim_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_anim_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_refr_now(NULL);

    /* Pixels the first frame leaves transparent show the GIF's own background. */
    {
        int x = pl->ox, y = pl->oy, w = cw, h = ch;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > LCD_H_RES) { w = LCD_H_RES - x; }
        if (y + h > LCD_V_RES) { h = LCD_V_RES - y; }
        display_hw_fill(x, y, w, h, gif_bg);
    }
    pl->fdisp = 0;
    more = anim_next_frame(pl, &ms);
    pl->ending = (more == 0 && !loop);   /* a one-frame GIF shown once */
    pl->timer = lv_timer_create(anim_timer_cb, (uint32_t)ms, pl);
    if (!pl->timer) {
        anim_close_locked();
        return ESP_ERR_NO_MEM;
    }
    strlcpy(s_anim_id, id, sizeof(s_anim_id));
    s_anim_loop = loop;
    s_anim_on = true;
    ESP_LOGI(TAG, "animation '%s' (%dx%d, %s, %lu bytes) -- eyes paused", id, cw, ch,
             loop ? "loop" : "once", (unsigned long)size);
    return ESP_OK;
}

