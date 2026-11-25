/* shim_esp.h -- the ESP-IDF / FreeRTOS calls the ported ESP code makes, on the
 * UNO Q's Zephyr core.
 *
 * tools/port_from_esp.py points every ESP-IDF and FreeRTOS #include of the copied
 * files at this one header, so robot.cpp, driver_board.c and the HTTP routes
 * compile unchanged. What each piece becomes:
 *
 *   ESP_LOGx               a line queued for Linux (shim_esp.cpp), shown in the
 *                          app log; ESP_LOGD/V are compiled out as on the ESP's
 *                          default log level
 *   esp_timer / delays     Zephyr's uptime and k_msleep / busy-wait
 *   tasks                  Zephyr threads, stacks from the heap; notify = k_sem
 *   mutex / semaphores     k_mutex / k_sem, queues k_msgq, critical = the
 *                          scheduler lock; esp_timer = one timer thread
 *   NVS                    a small key-value store in RAM, loaded from Linux at
 *                          boot (python/nvs.json) and saved back by Linux
 *   gpio                   no-ops: this robot has no servo power switch
 *   sdkconfig              the values the ESP build uses (Kconfig defaults)
 */
#ifndef SHIM_ESP_H
#define SHIM_ESP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── sdkconfig (main/Kconfig.projbuild defaults, as the ESP build has them) ── */
#ifndef CONFIG_APP_SERVO_HOLD_CURRENT_MA
#define CONFIG_APP_SERVO_HOLD_CURRENT_MA 50
#endif
#ifndef CONFIG_APP_SERVO_BAUD_RATE
#define CONFIG_APP_SERVO_BAUD_RATE 500000
#endif
#ifndef CONFIG_APP_SERVO_SPI_KHZ
#define CONFIG_APP_SERVO_SPI_KHZ 4000      /* 4 MHz on the carrier board (the ESP: 12); /unoq/spi?khz= changes it live */
#endif
#ifndef CONFIG_APP_SERVO_SPI_DRIVE
#define CONFIG_APP_SERVO_SPI_DRIVE 1       /* SCK/MOSI edge speed (OSPEEDR) 0 soft .. 3 sharp */
#endif
#define CONFIG_APP_PI_LINK_ENABLE 0
/* the RL runner (main/Kconfig.projbuild defaults; weights: Linux runs the file's floats) */
#define CONFIG_APP_POLICY_ENABLE 1
#define CONFIG_APP_POLICY_ROBOT "sku1"
#define CONFIG_APP_POLICY_INTERNAL_RESERVE_KB 96
#define CONFIG_APP_POLICY_TEMP_GUARD 0
#define CONFIG_APP_POLICY_TEMP_STOP_C 85
#define CONFIG_APP_POLICY_TEMP_IN_GETUP 0
#define CONFIG_APP_POLICY_AUTO_RIGHT 0
#define CONFIG_APP_POLICY_WEIGHT_BITS 32

/* ── esp_err ──────────────────────────────────────────────────────────────── */
typedef int esp_err_t;
#define ESP_OK                 0
#define ESP_FAIL              -1
#define ESP_ERR_NO_MEM         0x101
#define ESP_ERR_INVALID_ARG    0x102
#define ESP_ERR_INVALID_STATE  0x103
#define ESP_ERR_INVALID_SIZE   0x104
#define ESP_ERR_NOT_FOUND      0x105
#define ESP_ERR_TIMEOUT        0x107
#define ESP_ERR_NOT_SUPPORTED  0x106
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_INVALID_CRC    0x109
#define ESP_ERR_NOT_FINISHED   0x10C
#define ESP_ERR_NOT_ALLOWED    0x10D
#define ESP_ERR_NVS_NOT_FOUND  0x1102
#define ESP_ERROR_CHECK(x)     do { (void)(x); } while (0)
const char *esp_err_to_name(esp_err_t e);

/* ── logging ──────────────────────────────────────────────────────────────── */
void mpx_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(tag, ...) mpx_log('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) mpx_log('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) mpx_log('I', tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) do { if (0) mpx_log('D', tag, __VA_ARGS__); } while (0)
#define ESP_LOGV(tag, ...) do { if (0) mpx_log('V', tag, __VA_ARGS__); } while (0)
uint32_t esp_log_timestamp(void);

/* ── time ─────────────────────────────────────────────────────────────────── */
int64_t  esp_timer_get_time(void);          /* microseconds since boot */
void     esp_rom_delay_us(uint32_t us);     /* busy-wait */
uint32_t esp_cpu_get_cycle_count(void);
int      esp_cpu_get_core_id(void);
uint32_t esp_rom_get_cpu_ticks_per_us(void);

/* ── FreeRTOS ─────────────────────────────────────────────────────────────── */
typedef uint32_t TickType_t;
typedef int      BaseType_t;
typedef unsigned UBaseType_t;
typedef struct mpx_task *TaskHandle_t;
typedef struct mpx_sem  *SemaphoreHandle_t;
typedef struct mpx_queue *QueueHandle_t;
typedef void   (*TaskFunction_t)(void *);
typedef int      portMUX_TYPE;
typedef struct { uint32_t unused; } StaticSemaphore_t;
#define pdPASS                 1
#define pdFAIL                 0
#define pdTRUE                 1
#define pdFALSE                0
#define portMAX_DELAY          0xffffffffu
#define configTICK_RATE_HZ     1000
#define configMAX_PRIORITIES   25
#define portTICK_PERIOD_MS     1
#define tskNO_AFFINITY         (-1)
#define pdMS_TO_TICKS(ms)      ((TickType_t)(ms))      /* 1 ms ticks, as the ESP build */
#define pdTICKS_TO_MS(t)       ((uint32_t)(t))
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m)  mpx_critical_enter()
#define portEXIT_CRITICAL(m)   mpx_critical_exit()
#define portENTER_CRITICAL_ISR(m) mpx_critical_enter()
#define portEXIT_CRITICAL_ISR(m)  mpx_critical_exit()
#define taskENTER_CRITICAL(m)  mpx_critical_enter()
#define taskEXIT_CRITICAL(m)   mpx_critical_exit()
#define portYIELD()            vTaskDelay(0)
#define taskYIELD()            vTaskDelay(0)

/* Tasks are Zephyr threads with their stack from the heap. ESP priorities
 * (0..24, higher runs first) map to Zephyr's (lower runs first): 20+ (the gait,
 * the policy) -> 4, above the Bridge thread (5); 4..19 -> 6; below -> 8.
 * The main loop (14) stays the lowest. */
void         vTaskDelay(TickType_t ticks);
TickType_t   xTaskGetTickCount(void);
BaseType_t   xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack,
                                     void *arg, UBaseType_t prio, TaskHandle_t *out, BaseType_t core);
#define xTaskCreate(fn, name, stack, arg, prio, out) \
        xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, tskNO_AFFINITY)
#define xTaskCreateWithCaps(fn, name, stack, arg, prio, out, caps) \
        xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, tskNO_AFFINITY)
#define xTaskCreatePinnedToCoreWithCaps(fn, name, stack, arg, prio, out, core, caps) \
        xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, core)
void         vTaskDelete(TaskHandle_t h);      /* NULL: the calling task ends here */
#define vTaskDeleteWithCaps(h) vTaskDelete(h)
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void         xTaskNotifyGive(TaskHandle_t h);
uint32_t     ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);

SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
#define xSemaphoreCreateMutexStatic(buf)   ((void)(buf), xSemaphoreCreateMutex())
#define xSemaphoreCreateBinaryStatic(buf)  ((void)(buf), xSemaphoreCreateBinary())
BaseType_t   xSemaphoreTake(SemaphoreHandle_t m, TickType_t ticks);
BaseType_t   xSemaphoreGive(SemaphoreHandle_t m);
#define xSemaphoreTakeRecursive(m, t) xSemaphoreTake(m, t)
#define xSemaphoreGiveRecursive(m)    xSemaphoreGive(m)
#define xSemaphoreGiveFromISR(m, w)   xSemaphoreGive(m)
void         vSemaphoreDelete(SemaphoreHandle_t m);

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size);
BaseType_t   xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks);
#define xQueueSendToBack(q, i, t) xQueueSend(q, i, t)
BaseType_t   xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks);
UBaseType_t  uxQueueMessagesWaiting(QueueHandle_t q);
void         vQueueDelete(QueueHandle_t q);

void         mpx_critical_enter(void);
void         mpx_critical_exit(void);

/* ── esp_timer: callbacks run on one timer thread (Zephyr priority 3) ────── */
typedef struct mpx_timer *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *arg);
typedef enum { ESP_TIMER_TASK = 0, ESP_TIMER_ISR = 1 } esp_timer_dispatch_t;
typedef struct {
    esp_timer_cb_t callback;
    void *arg;
    esp_timer_dispatch_t dispatch_method;
    const char *name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out);
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t us);
esp_err_t esp_timer_restart(esp_timer_handle_t t, uint64_t us);
esp_err_t esp_timer_stop(esp_timer_handle_t t);
esp_err_t esp_timer_delete(esp_timer_handle_t t);
bool      esp_timer_is_active(esp_timer_handle_t t);

/* ── heap_caps: one heap here (the C library's, all the RAM the sketch leaves) */
#define MALLOC_CAP_EXEC       (1u << 0)
#define MALLOC_CAP_32BIT      (1u << 1)
#define MALLOC_CAP_8BIT       (1u << 2)
#define MALLOC_CAP_DMA        (1u << 3)
#define MALLOC_CAP_SPIRAM     (1u << 10)
#define MALLOC_CAP_INTERNAL   (1u << 11)
#define MALLOC_CAP_DEFAULT    (1u << 12)
void  *heap_caps_malloc(size_t n, uint32_t caps);
void  *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void  *heap_caps_realloc(void *p, size_t n, uint32_t caps);
void  *heap_caps_aligned_alloc(size_t align, size_t n, uint32_t caps);
void  *heap_caps_aligned_calloc(size_t align, size_t n, size_t size, uint32_t caps);
void   heap_caps_free(void *p);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t esp_get_free_heap_size(void);

/* ── random ───────────────────────────────────────────────────────────────── */
uint32_t esp_random(void);
void     esp_fill_random(void *buf, size_t len);

/* ── NVS ──────────────────────────────────────────────────────────────────────
 * One store, kept by Linux (python/nvs.json). Namespace "robot" keeps its keys
 * as they are (the calibration of v1..v4); any other namespace stores
 * "<ns>.<key>". Integers of every width are kept as int32. */
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out);
void      nvs_close(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_set_i32(nvs_handle_t h, const char *key, int32_t v);
esp_err_t nvs_get_i32(nvs_handle_t h, const char *key, int32_t *v);
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v);
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *v);
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v);
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v);
esp_err_t nvs_set_i8(nvs_handle_t h, const char *key, int8_t v);
esp_err_t nvs_get_i8(nvs_handle_t h, const char *key, int8_t *v);
esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t v);
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *v);
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *v);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *v, size_t *n);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t n);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *v, size_t *n);

/* the Linux side of the store (sketch.ino / shim_esp.cpp) */
bool      mpx_nvs_load(const char *key, uint8_t type, const void *v, size_t n);   /* type 'i' or 'b' */
bool      mpx_nvs_take_dirty(char *key, size_t keylen, uint8_t *type, uint8_t *v, size_t *n);
size_t    mpx_nvs_snapshot(uint8_t *buf, size_t max);                           /* the table as bytes */
bool      mpx_nvs_restore(const uint8_t *buf, size_t n);                        /* ... and back */
bool      mpx_log_take(char *line, size_t n);

/* ── gpio / SPI: what driver_board.c's boot check uses (spi_port.cpp) ──────
 * The pins are Arduino pin numbers here (board_config.h). Pull-ups and
 * pull-downs act on the servo bus's MISO (the boot check's "is anybody
 * talking" test); the other gpio calls have nothing to do on this board. */
typedef int gpio_num_t;
typedef int gpio_drive_cap_t;
#define GPIO_NUM_NC          (-1)
#define GPIO_MODE_OUTPUT     1
#define GPIO_MODE_INPUT      0
static inline esp_err_t gpio_set_level(gpio_num_t p, uint32_t v) { (void)p; (void)v; return ESP_OK; }
static inline esp_err_t gpio_set_direction(gpio_num_t p, int m) { (void)p; (void)m; return ESP_OK; }
static inline esp_err_t gpio_set_drive_capability(gpio_num_t p, gpio_drive_cap_t c) { (void)p; (void)c; return ESP_OK; }
esp_err_t gpio_pullup_en(gpio_num_t p);
esp_err_t gpio_pullup_dis(gpio_num_t p);
esp_err_t gpio_pulldown_en(gpio_num_t p);
esp_err_t gpio_pulldown_dis(gpio_num_t p);

typedef struct mpx_spi_dev *spi_device_handle_t;
typedef int spi_host_device_t;
typedef struct {
    int mode, duty_cycle_pos, clock_speed_hz, queue_size, spics_io_num;
    uint32_t flags;
} spi_device_interface_config_t;
typedef struct {
    uint32_t flags;
    size_t length;          /* bits */
    size_t rxlength;
    const void *tx_buffer;
    void *rx_buffer;
} spi_transaction_t;
esp_err_t spi_bus_add_device(spi_host_device_t host, const spi_device_interface_config_t *cfg, spi_device_handle_t *out);
esp_err_t spi_bus_remove_device(spi_device_handle_t h);
esp_err_t spi_device_transmit(spi_device_handle_t h, spi_transaction_t *t);

#ifdef __cplusplus
}
#endif

/* ── stdio files: none on the MCU (tools/port_from_esp.py no_files) ──────── */
#include <stdio.h>
static inline FILE  *mpx_fopen(const char *p, const char *m) { (void)p; (void)m; return NULL; }
static inline int    mpx_fclose(FILE *f) { (void)f; return 0; }
static inline int    mpx_fprintf(FILE *f, const char *fmt, ...) { (void)f; (void)fmt; return 0; }
static inline int    mpx_fputs(const char *s, FILE *f) { (void)s; (void)f; return 0; }
static inline int    mpx_fputc(int c, FILE *f) { (void)f; return c; }
static inline size_t mpx_fread(void *b, size_t s, size_t n, FILE *f) { (void)b; (void)s; (void)n; (void)f; return 0; }
static inline size_t mpx_fwrite(const void *b, size_t s, size_t n, FILE *f) { (void)b; (void)s; (void)n; (void)f; return 0; }
static inline int    mpx_fflush(FILE *f) { (void)f; return 0; }

#endif
