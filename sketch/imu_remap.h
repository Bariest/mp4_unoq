/* imu_remap.h -- the IMU's mounting on this carrier board
 *
 * Everything that reads the IMU (the RL policies' imu_R, the get-up and fall
 * rules, the face's tilt) was made for the BMI270 as it sits on firmware-dev's
 * ESP32 board. This carrier board can hold it turned another way. imu.cpp
 * (port_from_esp.py) passes every sample through imu_board_remap(), which
 * turns it into the ESP board's frame:
 *
 *   out[i] = sign * raw[axis]    with the map  { +-1, +-2, +-3 }  per output axis
 *
 * { 1, 2, 3 } = the same as the ESP board. The default is board_config.h's
 * IMU_MAP_*; a saved setting ("imu.map", learned from two poses with
 * /unoq/imu/learn, python/esp_api.py) overrides it.
 * The magnetometer (BMM150) is left as it is: nothing on the robot steers by it.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void mpx_imu_remap3(float *x, float *y, float *z);      /* in place, by the current map */
void mpx_imu_raw_note(float ax, float ay, float az);    /* the last raw accel, for /unoq/imu */
void mpx_imu_raw_last(float out[3]);
void mpx_imu_map_get(int8_t out[3]);
bool mpx_imu_map_set(int a, int b, int c);              /* checks it is a signed permutation, saves it */

#ifdef __cplusplus
}

template <class T> inline void imu_board_remap(T &s)
{
    mpx_imu_raw_note(s.ax, s.ay, s.az);
    mpx_imu_remap3(&s.ax, &s.ay, &s.az);
    mpx_imu_remap3(&s.gx, &s.gy, &s.gz);
}
#endif
