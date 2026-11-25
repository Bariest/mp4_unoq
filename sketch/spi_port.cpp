/* spi_port.cpp -- the servo bus on the UNO Q: SPI2 on D13 (SCK), D11 (MOSI),
 * D12 (MISO), one chip select per driver board (board_config.h).
 *
 * Found on this robot, none of which the ESP needs:
 *
 *  1. D11 is not MOSI until we say so. In the zephyr core 0.51 devicetree the
 *     timer TIM1 claims PB15 (= D11) for PWM and is set up after SPI2, so the
 *     MOSI signal never reached the pin while SCK and MISO worked - the boards
 *     answered, they just never heard a command. PB13..15 are put back on SPI2
 *     (alternate function 5) by register writes below.
 *  2. D10 is SPI2's hardware chip select (spi2_nss_pb9). SPI.begin() hands it to
 *     the peripheral; the chip selects are taken back as plain outputs after it.
 *  3. Edge speed. On the first wiring (long wires to the ESP board) sharp edges
 *     rang and the boards counted extra clocks (25 % bad replies at edge 1, 75 %
 *     at edge 2, 0 % at edge 0, 5 MHz). The carrier board's short traces allow
 *     4 MHz (CONFIG_APP_SERVO_SPI_KHZ / _DRIVE in shim_esp.h; the ESP runs 12);
 *     /unoq/spi?edge=&khz= changes both live, and the ESP's boot check
 *     (driver_board_boot_check) reports damaged replies per leg.
 *
 * Also here: the ESP-IDF calls of that boot check (spi_bus_add_device /
 * spi_device_transmit / the MISO pull-up and pull-down), on the same bus.
 */
#include <Arduino.h>
#include <SPI.h>
#include <string.h>
#include "board_config.h"
#include "shim_esp.h"
#include "spi_port.h"

#define CS_SETUP_US  1
#define CS_HOLD_US   1

struct mpx_spi_dev { int cs; int hz; };

namespace {
const uint8_t CS_PINS[4] = { SERVO_BOARD0_CS_PIN, SERVO_BOARD1_CS_PIN, SERVO_BOARD2_CS_PIN, SERVO_BOARD3_CS_PIN };
uint32_t s_hz = CONFIG_APP_SERVO_SPI_KHZ * 1000UL;
int s_edge = CONFIG_APP_SERVO_SPI_DRIVE;
uint32_t s_frames = 0;
SPISettings s_settings(CONFIG_APP_SERVO_SPI_KHZ * 1000UL, MSBFIRST, SPI_MODE0);
bool s_begun = false;

#if defined(ARDUINO_UNO_Q)
volatile uint32_t *const GPIOB_REGS = reinterpret_cast<volatile uint32_t *>(0x42020400UL);
#endif

void pins_to_spi2()
{
#if defined(ARDUINO_UNO_Q)
    volatile uint32_t *moder = GPIOB_REGS + 0, *ospeedr = GPIOB_REGS + 2, *afrh = GPIOB_REGS + 9;
    for (int pin = 13; pin <= 15; ++pin) {
        const int a = (pin - 8) * 4;
        *afrh = (*afrh & ~(0xFu << a)) | (5u << a);                       /* AF5 = SPI2 */
        if (pin != 14)                                                    /* MISO is an input */
            *ospeedr = (*ospeedr & ~(3u << (2 * pin))) | ((uint32_t)(s_edge & 3) << (2 * pin));
        *moder = (*moder & ~(3u << (2 * pin))) | (2u << (2 * pin));       /* alternate function */
    }
#endif
}

/* MISO (PB14) pull: 0 none, 1 up, 2 down */
void miso_pull(int pull)
{
#if defined(ARDUINO_UNO_Q)
    volatile uint32_t *pupdr = GPIOB_REGS + 3;
    *pupdr = (*pupdr & ~(3u << 28)) | ((uint32_t)(pull & 3) << 28);
#else
    (void)pull;
#endif
}

bool xfer_cs(int cs, const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (len > 64) return false;
    uint8_t buf[64];
    memcpy(buf, tx, len);
    SPI.beginTransaction(s_settings);
    if (cs >= 0) digitalWrite(cs, LOW);
    delayMicroseconds(CS_SETUP_US);
    SPI.transfer(buf, len);
    delayMicroseconds(CS_HOLD_US);
    if (cs >= 0) digitalWrite(cs, HIGH);
    SPI.endTransaction();
    if (rx) memcpy(rx, buf, len);
    ++s_frames;
    return true;
}
}  // namespace

extern "C" {

bool mpx_spi_begin(void)
{
    if (s_begun) return true;
    for (int b = 0; b < 4; ++b) { pinMode(CS_PINS[b], OUTPUT); digitalWrite(CS_PINS[b], HIGH); }
    SPI.begin();
    pins_to_spi2();
    for (int b = 0; b < 4; ++b) { pinMode(CS_PINS[b], OUTPUT); digitalWrite(CS_PINS[b], HIGH); }
    miso_pull(1);                          /* nobody talking reads 0xFF, as on the ESP */
    s_begun = true;
    return true;
}

bool mpx_spi_xfer(uint8_t board, const uint8_t *tx, uint8_t *rx, uint8_t len)
{
    if (board > 3) return false;
    return xfer_cs(CS_PINS[board], tx, rx, len);
}

uint32_t mpx_spi_hz(void) { return s_hz; }
int      mpx_spi_edge(void) { return s_edge; }
uint32_t mpx_spi_frames(void) { return s_frames; }

void mpx_spi_set(int edge, uint32_t hz)
{
    if (edge >= 0 && edge <= 3) s_edge = edge;
    if (hz >= 1000000UL && hz <= 20000000UL) { s_hz = hz; s_settings = SPISettings(hz, MSBFIRST, SPI_MODE0); }
    pins_to_spi2();
}

uint32_t mpx_spi_mosi_state(void)
{
#if defined(ARDUINO_UNO_Q)
    return ((GPIOB_REGS[0] >> 30) & 3u) * 100u + ((GPIOB_REGS[9] >> 28) & 0xFu);
#else
    return 205;
#endif
}

/* ── the ESP-IDF calls of driver_board_boot_check() ─────────────────────── */
esp_err_t spi_bus_add_device(spi_host_device_t, const spi_device_interface_config_t *cfg, spi_device_handle_t *out)
{
    spi_device_handle_t h = new mpx_spi_dev{ cfg->spics_io_num, cfg->clock_speed_hz };
    if (h->cs >= 0) mpx_spi_set(-1, (uint32_t)cfg->clock_speed_hz);   /* one clock for the bus */
    *out = h;
    return ESP_OK;
}

esp_err_t spi_bus_remove_device(spi_device_handle_t h)
{
    delete h;
    return ESP_OK;
}

esp_err_t spi_device_transmit(spi_device_handle_t h, spi_transaction_t *t)
{
    if (!h || !t) return ESP_ERR_INVALID_ARG;
    const size_t n = (t->length + 7) / 8;
    return xfer_cs(h->cs, (const uint8_t *)t->tx_buffer, (uint8_t *)t->rx_buffer, n) ? ESP_OK : ESP_FAIL;
}

esp_err_t gpio_pullup_en(gpio_num_t p)    { if (p == SERVO_SPI_MISO_PIN) miso_pull(1); return ESP_OK; }
esp_err_t gpio_pullup_dis(gpio_num_t p)   { if (p == SERVO_SPI_MISO_PIN) miso_pull(0); return ESP_OK; }
esp_err_t gpio_pulldown_en(gpio_num_t p)  { if (p == SERVO_SPI_MISO_PIN) miso_pull(2); return ESP_OK; }
esp_err_t gpio_pulldown_dis(gpio_num_t p) { if (p == SERVO_SPI_MISO_PIN) miso_pull(0); return ESP_OK; }

}  // extern "C"
