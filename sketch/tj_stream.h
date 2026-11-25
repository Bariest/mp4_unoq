/* tj_stream.h -- TrajOpt / Character move rows streamed from Linux.
 *
 * The ESP keeps ~600 KB of these tables in flash (trajopt_jump.h); the UNO Q's
 * sketch has room for their names and timing only (tj_meta.h). While a move
 * plays, tj_play() (moves_mpe.inc) reads its rows from here, and the Linux side
 * (python/tj_server.py) keeps the buffer filled from python/tj_tables.bin. */
#ifndef TJ_STREAM_H
#define TJ_STREAM_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* gait task side */
bool           tj_stream_open(int table, int frames);   /* waits for the first rows (<= 3 s) */
const int16_t *tj_stream_row(int frame);                /* 12 x deg10; waits <= 150 ms; NULL = gone */
const int16_t *tj_stream_first(void);                   /* row 0 (kept from the prefetch) */
const int16_t *tj_stream_last(void);                    /* the table's last row (the stand) */
void           tj_stream_close(void);

/* Bridge side (sketch.ino) */
bool tj_stream_wanted(int *table, int *next_row, int *space);   /* what Linux should send now */
int  tj_stream_push(int table, int start, const uint8_t *rows, int nbytes);   /* rows accepted, -1 = refused */

#ifdef __cplusplus
}
#endif
#endif
