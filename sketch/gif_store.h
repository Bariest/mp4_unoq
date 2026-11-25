/* gif_store.h -- the one GIF the face can play, in RAM: the files live on
 * Linux (python/display_api.py), which sends the chosen one here first. */
#ifndef GIF_STORE_H
#define GIF_STORE_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* the file of animation `id` when it is complete in RAM, else NULL */
uint8_t *mpx_gif_get(const char *id, uint32_t *size);
/* ask Linux to send it (the boot-time restore); at most every few seconds */
void     mpx_gif_want(const char *id);
bool     mpx_gif_take_want(char *id, int n);       /* for the sketch's loop: what to ask for */
/* from Linux (sketch.ino): begin (frees the old file), the bytes, end */
bool     mpx_gif_begin(const char *id, uint32_t size);
bool     mpx_gif_put(uint32_t offset, const uint8_t *data, uint32_t n);
bool     mpx_gif_end(const char *id);
const char *mpx_gif_loaded(void);                   /* "" when none */
/* cap_display_face.c: let go of the file before it is replaced */
void     cap_display_anim_release_file(void);
#ifdef __cplusplus
}
#endif
#endif
