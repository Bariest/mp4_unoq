/* imu_remap.cpp -- see imu_remap.h */
#include <stdlib.h>
#include "shim_esp.h"
#include "imu_remap.h"
#include "board_config.h"

namespace {

int8_t  s_map[3] = { IMU_MAP_X, IMU_MAP_Y, IMU_MAP_Z };   /* board_config.h, until a saved map */
bool    s_loaded = false;
float   s_raw[3] = { 0, 0, 0 };

bool valid(int a, int b, int c)
{
    const int m[3] = { a, b, c };
    bool seen[3] = { false, false, false };
    for (int i = 0; i < 3; ++i) {
        const int k = abs(m[i]);
        if (k < 1 || k > 3 || seen[k - 1]) return false;
        seen[k - 1] = true;
    }
    return true;
}

void load_once()
{
    if (s_loaded) return;
    s_loaded = true;
    nvs_handle_t h;
    if (nvs_open("imu", NVS_READONLY, &h) != ESP_OK) return;
    int8_t m[3];
    size_t n = sizeof m;
    if (nvs_get_blob(h, "map", m, &n) == ESP_OK && n == sizeof m && valid(m[0], m[1], m[2])) {
        s_map[0] = m[0]; s_map[1] = m[1]; s_map[2] = m[2];
    }
    nvs_close(h);
}

}  // namespace

extern "C" void mpx_imu_remap3(float *x, float *y, float *z)
{
    load_once();
    if (s_map[0] == 1 && s_map[1] == 2 && s_map[2] == 3) return;
    const float in[3] = { *x, *y, *z };
    float out[3];
    for (int i = 0; i < 3; ++i) {
        const int k = abs(s_map[i]) - 1;
        out[i] = s_map[i] < 0 ? -in[k] : in[k];
    }
    *x = out[0]; *y = out[1]; *z = out[2];
}

extern "C" void mpx_imu_raw_note(float ax, float ay, float az) { s_raw[0] = ax; s_raw[1] = ay; s_raw[2] = az; }
extern "C" void mpx_imu_raw_last(float out[3]) { out[0] = s_raw[0]; out[1] = s_raw[1]; out[2] = s_raw[2]; }

extern "C" void mpx_imu_map_get(int8_t out[3])
{
    load_once();
    out[0] = s_map[0]; out[1] = s_map[1]; out[2] = s_map[2];
}

extern "C" bool mpx_imu_map_set(int a, int b, int c)
{
    if (!valid(a, b, c)) return false;
    load_once();
    s_map[0] = (int8_t)a; s_map[1] = (int8_t)b; s_map[2] = (int8_t)c;
    nvs_handle_t h;
    if (nvs_open("imu", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "map", s_map, sizeof s_map);
        nvs_commit(h);
        nvs_close(h);
    }
    return true;
}
