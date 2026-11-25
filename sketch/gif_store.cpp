/* gif_store.cpp -- see gif_store.h */
#include <Arduino.h>
#include <stdlib.h>
#include <string.h>
#include "shim_esp.h"
#include "gif_store.h"

namespace {
const char *TAG = "gif_store";
uint8_t *s_data = nullptr;
uint32_t s_size = 0, s_got = 0;
bool     s_complete = false;
char     s_id[32] = "";
char     s_want[32] = "";
uint32_t s_want_ms = 0;
bool     s_want_new = false;
}

extern "C" {

uint8_t *mpx_gif_get(const char *id, uint32_t *size)
{
    if (!s_complete || !id || strcmp(id, s_id) != 0) return nullptr;
    if (size) *size = s_size;
    return s_data;
}

void mpx_gif_want(const char *id)
{
    if (!id || !id[0]) return;
    mpx_critical_enter();
    const uint32_t now = millis();
    if (strcmp(id, s_want) != 0 || now - s_want_ms > 5000) {
        strncpy(s_want, id, sizeof s_want - 1);
        s_want[sizeof s_want - 1] = 0;
        s_want_ms = now;
        s_want_new = true;
    }
    mpx_critical_exit();
}

bool mpx_gif_take_want(char *id, int n)
{
    mpx_critical_enter();
    const bool w = s_want_new;
    if (w) {
        strncpy(id, s_want, n - 1);
        id[n - 1] = 0;
        s_want_new = false;
    }
    mpx_critical_exit();
    return w;
}

bool mpx_gif_begin(const char *id, uint32_t size)
{
    if (!id || !id[0] || strlen(id) >= sizeof s_id || size < 13) return false;
    s_complete = false;                      /* nothing new starts on the old one ... */
    cap_display_anim_release_file();         /* ... and the player lets go of it */
    free(s_data);
    s_data = nullptr;
    s_id[0] = 0;
    s_size = s_got = 0;
    s_data = static_cast<uint8_t *>(malloc(size));
    if (!s_data) {
        ESP_LOGW(TAG, "no %lu bytes free for %s.gif (largest block %lu)", (unsigned long)size, id,
                 (unsigned long)heap_caps_get_largest_free_block(0));
        return false;
    }
    strcpy(s_id, id);
    s_size = size;
    return true;
}

bool mpx_gif_put(uint32_t offset, const uint8_t *data, uint32_t n)
{
    if (!s_data || offset + n > s_size) return false;
    memcpy(s_data + offset, data, n);
    if (offset + n > s_got) s_got = offset + n;
    return true;
}

bool mpx_gif_end(const char *id)
{
    if (!s_data || strcmp(id, s_id) != 0 || s_got != s_size) return false;
    s_complete = true;
    ESP_LOGI(TAG, "%s.gif in RAM (%lu bytes)", s_id, (unsigned long)s_size);
    return true;
}

const char *mpx_gif_loaded(void) { return s_complete ? s_id : ""; }

}  // extern "C"
