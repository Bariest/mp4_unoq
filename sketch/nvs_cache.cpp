/* nvs_cache.cpp -- see nvs_cache.h
 *
 * Where: the core's storage_partition (0x081C0000, 256 KB, unused by the core);
 * only its first 8 KB page is used. Layout: a 16-byte header
 * { magic "MPXN", version, length, FNV-1a of the payload } and the payload of
 * mpx_nvs_snapshot(). The page is rewritten only when the table differs from
 * what is there (a few times per calibration session; the flash takes 10k).
 * The sketch runs from the same flash bank: the CPU waits a few ms while the
 * page is erased and written, which the gait does not notice.
 */
#include <Arduino.h>
#include <string.h>
#include <zephyr/storage/flash_map.h>
#include "shim_esp.h"
#include "nvs_cache.h"

namespace {

constexpr uint32_t MAGIC = 0x4E58504DUL;    /* "MPXN" */
constexpr uint32_t VERSION = 1;
constexpr size_t   PAGE = 8192, HDR = 16, CHUNK = 256;

alignas(16) uint8_t s_buf[PAGE];

uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

}  // namespace

bool nvs_cache_load()
{
    const struct flash_area *fa = nullptr;
    if (flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa) != 0) return false;
    uint32_t h[4] = { 0, 0, 0, 0 };
    bool ok = flash_area_read(fa, 0, h, sizeof h) == 0
              && h[0] == MAGIC && h[1] == VERSION && h[2] > 2 && h[2] <= PAGE - HDR
              && flash_area_read(fa, HDR, s_buf, h[2]) == 0
              && fnv1a(s_buf, h[2]) == h[3]
              && (s_buf[0] | (s_buf[1] << 8)) > 0           /* an empty table is no calibration */
              && mpx_nvs_restore(s_buf, h[2]);
    flash_area_close(fa);
    return ok;
}

int nvs_cache_save()
{
    memset(s_buf, 0xFF, sizeof s_buf);
    const size_t n = mpx_nvs_snapshot(s_buf + HDR, PAGE - HDR);
    if (n <= 2 || (s_buf[HDR] | (s_buf[HDR + 1] << 8)) == 0) return 0;
    const uint32_t h[4] = { MAGIC, VERSION, (uint32_t)n, fnv1a(s_buf + HDR, n) };
    memcpy(s_buf, h, sizeof h);
    const size_t total = (HDR + n + 15) & ~(size_t)15;      /* whole 16-byte flash words */

    const struct flash_area *fa = nullptr;
    if (flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa) != 0) return -1;
    bool same = true;
    uint8_t chunk[CHUNK];
    for (size_t o = 0; same && o < total; o += CHUNK) {
        const size_t k = total - o < CHUNK ? total - o : CHUNK;
        if (flash_area_read(fa, o, chunk, k) != 0 || memcmp(chunk, s_buf + o, k) != 0) same = false;
    }
    int r = 0;
    if (!same) {
        r = (flash_area_erase(fa, 0, PAGE) == 0 && flash_area_write(fa, 0, s_buf, total) == 0) ? 1 : -1;
    }
    flash_area_close(fa);
    return r;
}
