/* nvs_cache.h -- a copy of the saved settings in the MCU's own flash
 *
 * Linux keeps the settings (nvs.json) and sends them at every start. A copy of
 * the same table is kept in the STM32's "storage" flash partition, so after a
 * power-on the robot can stand up at once, without waiting a minute for Linux.
 */
#pragma once

bool nvs_cache_load();   /* true: a good copy was found and put into the settings table */
int  nvs_cache_save();   /* 1 written, 0 already the same (or nothing to keep), -1 flash error */
