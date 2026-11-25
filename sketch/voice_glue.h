/* voice_glue.h -- the voice front end the RL runner pauses during a run
 * (firmware-dev main/voice). The UNO Q robot has no microphones: nothing to
 * pause. */
#pragma once
#include "shim_esp.h"
namespace voice_glue {
inline esp_err_t hold(const char *) { return ESP_OK; }
inline void      release(const char *) {}
inline esp_err_t release_front_end() { return ESP_OK; }
}
