/* lv_conf.h -- LVGL 9.5 for the Mini Pupper face on the UNO Q (display_hw.cpp).
 * The ESP build's settings (sdkconfig.defaults: CONFIG_LV_*), with what this
 * robot does not use switched off so the sketch stays within its flash.
 * Everything not named here takes LVGL's default (lv_conf_internal.h). */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH              16
#define LV_DEF_REFR_PERIOD          33
#define LV_DPI_DEF                  130

/* memory from the C library (all the RAM the sketch leaves), LVGL's own
 * string and printf code: the sketch loader resolves only some libc symbols */
#define LV_USE_STDLIB_MALLOC        LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING        LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF       LV_STDLIB_BUILTIN
#define LV_SPRINTF_USE_FLOAT        0

#define LV_USE_OS                   LV_OS_NONE        /* display_hw.cpp holds a lock around LVGL */

#define LV_USE_DRAW_SW              1
#define LV_DRAW_SW_DRAW_UNIT_CNT    1
#define LV_USE_DRAW_SW_ASM          LV_DRAW_SW_ASM_NONE
#define LV_DRAW_SW_COMPLEX          1
#define LV_USE_DRAW_SW_COMPLEX_GRADIENTS 0
#define LV_DRAW_SW_SUPPORT_RGB565        1
#define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED 0
#define LV_DRAW_SW_SUPPORT_RGB565A8      1
#define LV_DRAW_SW_SUPPORT_RGB888        0
#define LV_DRAW_SW_SUPPORT_XRGB8888      0
#define LV_DRAW_SW_SUPPORT_ARGB8888      1
#define LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED 0
#define LV_DRAW_SW_SUPPORT_L8            0
#define LV_DRAW_SW_SUPPORT_AL88          0
#define LV_DRAW_SW_SUPPORT_A8            1
#define LV_DRAW_SW_SUPPORT_I1            0
#define LV_DRAW_BUF_ALIGN           4
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE (16 * 1024)
#define LV_USE_DRAW_DMA2D           0
#define LV_USE_NATIVE_HELIUM_ASM    0
#define LV_USE_VECTOR_GRAPHIC       0
#define LV_USE_MATRIX               0
#define LV_USE_FLOAT                0

#define LV_USE_LOG                  0
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0
#define LV_USE_REFR_DEBUG           0
#define LV_USE_LAYER_DEBUG          0
#define LV_USE_PARALLEL_DRAW_DEBUG  0
#define LV_USE_OBJ_ID               0
#define LV_USE_OBJ_NAME             0
#define LV_USE_OBJ_PROPERTY         0
#define LV_USE_EXT_DATA             0
#define LV_CACHE_DEF_SIZE           0
#define LV_IMAGE_HEADER_CACHE_DEF_CNT 0
#define LV_GRADIENT_MAX_STOPS       2

/* fonts: the ESP's 14 (default). Its 28 and 48 only served free drawing
 * (cap_display_draw, an agent / Lua feature the UNO Q has not got): left out,
 * the sketch has 768 KB of flash for everything */
#define LV_FONT_MONTSERRAT_14       1
#define LV_FONT_MONTSERRAT_14_ALIGNED 0
#define LV_FONT_MONTSERRAT_28       0
#define LV_FONT_MONTSERRAT_48       0
#define LV_FONT_DEFAULT             &lv_font_montserrat_14
#define LV_FONT_FMT_TXT_LARGE       0
#define LV_USE_FONT_COMPRESSED      0
#define LV_USE_FONT_PLACEHOLDER     1
#define LV_TXT_ENC                  LV_TXT_ENC_UTF8
#define LV_USE_BIDI                 0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

/* widgets: what the face uses */
#define LV_WIDGETS_HAS_DEFAULT_VALUE 0
#define LV_USE_ANIMIMG     0
#define LV_USE_ARC         0
#define LV_USE_ARCLABEL    0
#define LV_USE_BAR         0
#define LV_USE_BUTTON      0
#define LV_USE_BUTTONMATRIX 0
#define LV_USE_CALENDAR    0
#define LV_USE_CANVAS      1
#define LV_USE_CHART       0
#define LV_USE_CHECKBOX    0
#define LV_USE_DROPDOWN    0
#define LV_USE_IMAGE       1
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_KEYBOARD    0
#define LV_USE_LABEL       1
#define LV_LABEL_TEXT_SELECTION 0
#define LV_LABEL_LONG_TXT_HINT 0
#define LV_USE_LED         0
#define LV_USE_LINE        1
#define LV_USE_LIST        0
#define LV_USE_LOTTIE      0
#define LV_USE_MENU        0
#define LV_USE_MSGBOX      0
#define LV_USE_ROLLER      0
#define LV_USE_SCALE       0
#define LV_USE_SLIDER      0
#define LV_USE_SPAN        0
#define LV_USE_SPINBOX     0
#define LV_USE_SPINNER     0
#define LV_USE_SWITCH      0
#define LV_USE_TABLE       0
#define LV_USE_TABVIEW     0
#define LV_USE_TEXTAREA    0
#define LV_USE_TILEVIEW    0
#define LV_USE_WIN         0
#define LV_USE_3DTEXTURE   0

#define LV_USE_THEME_DEFAULT        1
#define LV_THEME_DEFAULT_DARK       0
#define LV_THEME_DEFAULT_GROW       0
#define LV_USE_THEME_SIMPLE         0
#define LV_USE_THEME_MONO           0
#define LV_USE_FLEX                 1
#define LV_USE_GRID                 0

/* libraries: the GIF decoder (AnimatedGIF), fed from RAM (cap_display_face.c) */
#define LV_USE_GIF                  1
#define LV_GIF_CACHE_DECODE_DATA    0
#define LV_USE_FS_STDIO             0
#define LV_USE_FS_POSIX             0
#define LV_USE_FS_WIN32             0
#define LV_USE_FS_FATFS             0
#define LV_USE_FS_MEMFS             0
#define LV_USE_FS_LITTLEFS          0
#define LV_USE_FS_ARDUINO_ESP_LITTLEFS 0
#define LV_USE_FS_ARDUINO_SD        0
#define LV_USE_FS_UEFI              0
#define LV_USE_FS_FROGFS            0
#define LV_USE_LODEPNG              0
#define LV_USE_LIBPNG               0
#define LV_USE_BMP                  0
#define LV_USE_TJPGD                0
#define LV_USE_LIBJPEG_TURBO        0
#define LV_USE_LIBWEBP              0
#define LV_USE_RLE                  0
#define LV_USE_LZ4_INTERNAL         0
#define LV_USE_LZ4_EXTERNAL         0
#define LV_USE_QRCODE               0
#define LV_USE_BARCODE              0
#define LV_USE_FREETYPE             0
#define LV_USE_TINY_TTF             0
#define LV_USE_RLOTTIE              0
#define LV_USE_THORVG_INTERNAL      0
#define LV_USE_THORVG_EXTERNAL      0
#define LV_USE_SVG                  0
#define LV_USE_FFMPEG               0
#define LV_USE_GLTF                 0

#define LV_USE_SNAPSHOT             0
#define LV_USE_SYSMON               0
#define LV_USE_PROFILER             0
#define LV_USE_MONKEY               0
#define LV_USE_GRIDNAV              0
#define LV_USE_FRAGMENT             0
#define LV_USE_IMGFONT              0
#define LV_USE_OBSERVER             0
#define LV_USE_IME_PINYIN           0
#define LV_USE_FILE_EXPLORER        0
#define LV_USE_FONT_MANAGER         0
#define LV_USE_TEST                 0
#define LV_USE_XML                  0
#define LV_USE_TRANSLATION          0
#define LV_USE_COLOR_FILTER         0
#define LV_USE_GESTURE_RECOGNITION  0

#define LV_USE_SDL                  0
#define LV_USE_X11                  0
#define LV_USE_WAYLAND              0
#define LV_USE_LINUX_FBDEV          0
#define LV_USE_LINUX_DRM            0
#define LV_USE_NUTTX                0
#define LV_USE_ST7789               0
#define LV_USE_GENERIC_MIPI         0
#define LV_USE_LOVYAN_GFX           0
#define LV_USE_EVDEV                0
#define LV_USE_LIBINPUT             0
#define LV_USE_WINDOWS              0
#define LV_USE_OPENGLES             0
#define LV_USE_QNX                  0

#define LV_BUILD_EXAMPLES           0
#define LV_BUILD_DEMOS              0

#endif
