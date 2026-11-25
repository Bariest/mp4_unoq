/* tj_stream.cpp -- see tj_stream.h. A ring of RING_ROWS rows; Linux pushes rows
 * in order (start = the next row it was asked for), the player reads them by
 * frame number. A table that fits the ring is fetched whole before it starts,
 * so the 5 ms jumps never wait on the Bridge mid-air. */
#include "tj_stream.h"
#include "shim_esp.h"
#include <stdlib.h>
#include <string.h>

namespace {
int RING_ROWS = 544;                      /* 13 KB; the 5 ms jumps (504-519 rows) fit whole */
int16_t (*s_ring)[12] = nullptr;

bool ring_alloc()
{
    /* the heap is small and shared: take what there is, down to 2.5 s of 10 ms rows */
    static const int SIZES[3] = { 544, 384, 256 };
    for (int rows : SIZES) {
        s_ring = static_cast<int16_t (*)[12]>(malloc(sizeof(int16_t) * 12 * rows));
        if (s_ring) { RING_ROWS = rows; return true; }
    }
    return false;
}
volatile int s_table = -1;                /* -1 = closed */
volatile int s_frames = 0;
volatile int s_have = 0;                  /* rows 0..s_have-1 received */
volatile int s_read = 0;                  /* lowest row the player may still read */
volatile bool s_have_last = false;
int16_t s_last[12];
int16_t s_first[12];
}

extern "C" {

bool tj_stream_open(int table, int frames)
{
    if (!s_ring && !ring_alloc()) { ESP_LOGE("tj", "no memory for the row buffer"); return false; }
    if (frames <= 0) return false;
    mpx_critical_enter();
    s_frames = frames;
    s_have = 0;
    s_read = 0;
    s_have_last = false;
    s_table = table;
    mpx_critical_exit();
    const int want = frames < RING_ROWS - 1 ? frames : RING_ROWS - 1;   /* the ring keeps one slot free */
    for (int t = 0; t < 3000; ++t) {          /* up to 3 s for the prefetch */
        if (s_have >= want && s_have_last) {
            memcpy(s_first, s_ring[0], sizeof s_first);
            return true;
        }
        vTaskDelay(1);
    }
    ESP_LOGW("tj", "table %d: %d of %d rows after 3 s", table, s_have, want);
    s_table = -1;
    return false;
}

const int16_t *tj_stream_row(int f)
{
    if (s_table < 0 || f < 0 || f >= s_frames) return nullptr;
    for (int t = 0; t < 150 && f >= s_have; ++t) vTaskDelay(1);
    if (f >= s_have || f < s_have - RING_ROWS) return nullptr;
    if (f > s_read) s_read = f;               /* rows before f may be overwritten */
    return s_ring[f % RING_ROWS];
}

const int16_t *tj_stream_first(void) { return s_first; }
const int16_t *tj_stream_last(void) { return s_last; }

void tj_stream_close(void) { s_table = -1; }

bool tj_stream_wanted(int *table, int *next_row, int *space)
{
    const int k = s_table;
    if (k < 0 || !s_ring) return false;
    const int free_rows = RING_ROWS - (s_have - s_read) - 1;
    if ((s_have >= s_frames || free_rows <= 0) && s_have_last) return false;
    *table = k;
    *next_row = s_have_last ? s_have : -1;    /* -1: send the last row first */
    *space = free_rows > 0 ? free_rows : 0;
    return true;
}

int tj_stream_push(int table, int start, const uint8_t *rows, int nbytes)
{
    if (table != s_table || !s_ring || nbytes % 24) return -1;
    const int n = nbytes / 24;
    if (start == -1) {                        /* the last row */
        if (n < 1) return -1;
        memcpy(s_last, rows, 24);
        s_have_last = true;
        return 1;
    }
    if (start != s_have) return -1;           /* out of order: Linux re-asks */
    int ok = 0;
    for (int i = 0; i < n && s_have < s_frames; ++i) {
        if (s_have - s_read >= RING_ROWS - 1) break;   /* full */
        memcpy(s_ring[s_have % RING_ROWS], rows + 24 * i, 24);
        mpx_critical_enter();
        ++s_have;
        mpx_critical_exit();
        ++ok;
    }
    return ok;
}

}  // extern "C"
