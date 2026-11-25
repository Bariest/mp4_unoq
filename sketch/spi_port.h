/* spi_port.h -- the servo bus on the UNO Q (used by the ported driver_board.c) */
#ifndef SPI_PORT_H
#define SPI_PORT_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
bool     mpx_spi_begin(void);
/* one full-duplex frame to driver board 0..3 (rx may equal tx) */
bool     mpx_spi_xfer(uint8_t board, const uint8_t *tx, uint8_t *rx, uint8_t len);
uint32_t mpx_spi_hz(void);
int      mpx_spi_edge(void);
/* run-time tuning (Bridge mp_spi): edge 0..3 or -1 = keep, hz 2..12 MHz or 0 = keep */
void     mpx_spi_set(int edge, uint32_t hz);
uint32_t mpx_spi_mosi_state(void);   /* PB15 mode*100 + AF: 205 = SPI2 MOSI */
uint32_t mpx_spi_frames(void);
#ifdef __cplusplus
}
#endif
#endif
