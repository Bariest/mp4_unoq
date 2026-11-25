/* shim_i2c.cpp -- see shim_i2c.h. One bus (Wire), devices by address. */
#include <Arduino.h>
#include <Wire.h>
#include "board_config.h"
#include "shim_i2c.h"

struct mpx_i2c_bus { int unused; };
struct mpx_i2c_dev { uint8_t addr; };

namespace {
mpx_i2c_bus s_bus;
bool s_begun = false;
int s_bmi_addr = -1;

void bus_begin()
{
    if (s_begun) return;
    Wire.begin();
    Wire.setClock(IMU_I2C_FREQ_HZ);
    s_begun = true;
}

bool ack(uint8_t addr)
{
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}
}  // namespace

namespace hal {
i2c_master_bus_handle_t i2c_bus()
{
#if IMU_ENABLE
    bus_begin();
    return &s_bus;
#else
    return nullptr;
#endif
}
}  // namespace hal

extern "C" {

int mpx_bmi270_addr(void)
{
    if (s_bmi_addr < 0) {
        bus_begin();
        s_bmi_addr = ack(0x68) ? 0x68 : ack(0x69) ? 0x69 : 0x68;
    }
    return s_bmi_addr;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                                    i2c_master_dev_handle_t *out)
{
    if (!bus || !cfg || !out) return ESP_ERR_INVALID_ARG;
    *out = new mpx_i2c_dev{ static_cast<uint8_t>(cfg->device_address) };
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev)
{
    delete dev;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *buf, size_t n, int)
{
    if (!dev || n > 250) return ESP_ERR_INVALID_ARG;
    Wire.beginTransmission(dev->addr);
    Wire.write(buf, n);
    return Wire.endTransmission() == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t i2c_master_receive(i2c_master_dev_handle_t dev, uint8_t *rd, size_t rn, int)
{
    if (!dev || rn > 250) return ESP_ERR_INVALID_ARG;
    if (Wire.requestFrom(dev->addr, rn) != rn) return ESP_FAIL;
    for (size_t i = 0; i < rn; ++i) rd[i] = static_cast<uint8_t>(Wire.read());
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *wr, size_t wn,
                                      uint8_t *rd, size_t rn, int t)
{
    /* the core's Wire ends every write with a stop: the BMI270 does not mind */
    const esp_err_t e = i2c_master_transmit(dev, wr, wn, t);
    return e == ESP_OK ? i2c_master_receive(dev, rd, rn, t) : e;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t, uint16_t addr, int)
{
    bus_begin();
    return ack(static_cast<uint8_t>(addr)) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

}  // extern "C"
