/* shim_i2c.h -- the ESP-IDF i2c_master calls of imu.cc (and the shared bus of
 * platform/shared_i2c.h) on the UNO Q's Wire: I2C2 on D20 (SDA) / D21 (SCL),
 * where the carrier board has the BMI270 (and the BMM150 behind it). */
#ifndef SHIM_I2C_H
#define SHIM_I2C_H

#include <stdint.h>
#include <stddef.h>
#include "shim_esp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mpx_i2c_bus *i2c_master_bus_handle_t;
typedef struct mpx_i2c_dev *i2c_master_dev_handle_t;
typedef int i2c_port_t;
typedef enum { I2C_ADDR_BIT_LEN_7 = 0, I2C_ADDR_BIT_LEN_10 = 1 } i2c_addr_bit_len_t;
typedef struct {
    i2c_addr_bit_len_t dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
    uint32_t scl_wait_us;
    struct { uint32_t disable_ack_check : 1; } flags;
} i2c_device_config_t;

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                                    i2c_master_dev_handle_t *out);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *buf, size_t n, int timeout_ms);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *wr, size_t wn,
                                      uint8_t *rd, size_t rn, int timeout_ms);
esp_err_t i2c_master_receive(i2c_master_dev_handle_t dev, uint8_t *rd, size_t rn, int timeout_ms);
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t addr, int timeout_ms);

/* the BMI270's address on this board: 0x68 (SDO low), else 0x69 if only that answers */
int mpx_bmi270_addr(void);

/* gpio_config() of the IMU's interrupt pin: nothing listens to it (as on the ESP) */
typedef enum { GPIO_PULLUP_DISABLE = 0, GPIO_PULLUP_ENABLE = 1 } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE = 0, GPIO_PULLDOWN_ENABLE = 1 } gpio_pulldown_t;
typedef enum { GPIO_INTR_DISABLE = 0 } gpio_int_type_t;
typedef struct {
    uint64_t pin_bit_mask;
    int mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;
static inline esp_err_t gpio_config(const gpio_config_t *c) { (void)c; return ESP_OK; }

#ifdef __cplusplus
}
namespace hal { i2c_master_bus_handle_t i2c_bus(); }
#endif

#endif
