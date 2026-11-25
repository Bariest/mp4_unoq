/* shim_esp.cpp -- see shim_esp.h. Zephyr (the UNO Q) or, for the host test
 * build (tools/hosttest), the C++ standard library. */
#include "shim_esp.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(__ZEPHYR__)
#include <Arduino.h>
#include <zephyr/kernel.h>
#else
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <vector>
#endif
#include <stdlib.h>

namespace {

/* ── a lock, both builds ───────────────────────────────────────────────── */
#if defined(__ZEPHYR__)
struct Lock {
    struct k_mutex m;
    Lock() { k_mutex_init(&m); }
    void lock()   { k_mutex_lock(&m, K_FOREVER); }
    void unlock() { k_mutex_unlock(&m); }
};
#else
struct Lock {
    std::recursive_mutex m;
    void lock()   { m.lock(); }
    void unlock() { m.unlock(); }
};
#endif
struct Guard { Lock &l; explicit Guard(Lock &x) : l(x) { l.lock(); } ~Guard() { l.unlock(); } };

/* ── log ring: ESP_LOGx lines waiting for Linux ─────────────────────────── */
constexpr int LOG_LINES = 64, LOG_LEN = 160;
char     s_log[LOG_LINES][LOG_LEN];
int      s_log_head = 0, s_log_count = 0;
uint32_t s_log_dropped = 0;
Lock     s_log_lock;

/* ── NVS: key -> int32 or blob, loaded from Linux, dirty entries saved back ── */
constexpr int NVS_MAX = 96, NVS_KEY = 32, NVS_VAL = 40;
struct NvsEntry { char key[NVS_KEY]; uint8_t type; uint8_t len; bool dirty; uint8_t v[NVS_VAL]; };
NvsEntry s_nvs[NVS_MAX];
int      s_nvs_n = 0;
Lock     s_nvs_lock;

NvsEntry *nvs_find(const char *key)
{
    for (int i = 0; i < s_nvs_n; ++i)
        if (strncmp(s_nvs[i].key, key, NVS_KEY) == 0) return &s_nvs[i];
    return nullptr;
}

NvsEntry *nvs_put(const char *key, uint8_t type, const void *v, size_t n, bool dirty)
{
    if (strlen(key) >= NVS_KEY || n > NVS_VAL) return nullptr;
    NvsEntry *e = nvs_find(key);
    if (!e) {
        if (s_nvs_n >= NVS_MAX) return nullptr;
        e = &s_nvs[s_nvs_n++];
        memset(e, 0, sizeof *e);
        strncpy(e->key, key, NVS_KEY - 1);
    }
    const bool changed = e->type != type || e->len != n || memcmp(e->v, v, n) != 0;
    e->type = type;
    e->len = static_cast<uint8_t>(n);
    memcpy(e->v, v, n);
    if (dirty && changed) e->dirty = true;
    return e;
}

/* ── tasks ─────────────────────────────────────────────────────────────── */
}  // namespace
struct mpx_task {
#if defined(__ZEPHYR__)
    struct k_thread thread;
    struct k_sem notify;
    void *stack_mem;
#else
    std::mutex mu;
    std::condition_variable cv;
    uint32_t count = 0;
    std::thread::id id;
#endif
    TaskFunction_t fn;
    void *arg;
    char name[16];
    bool used;
};
namespace {
constexpr int MAX_TASKS = 16;
mpx_task s_tasks[MAX_TASKS];

mpx_task *task_self()
{
    for (auto &t : s_tasks) {
#if defined(__ZEPHYR__)
        if (t.used && &t.thread == k_sched_current_thread_query()) return &t;
#else
        if (t.used && t.id == std::this_thread::get_id()) return &t;
#endif
    }
    return nullptr;
}

#if defined(__ZEPHYR__)
void task_entry(void *p, void *, void *)
{
    mpx_task *t = static_cast<mpx_task *>(p);
    t->fn(t->arg);
    t->used = false;              /* returned: the slot is free (the stack is not: it is still in use) */
}
int zephyr_prio(UBaseType_t esp)
{
    if (esp >= 20) return 4;      /* gait, policy: above the Bridge (5) */
    if (esp >= 4) return 6;
    return 8;
}
#endif
}  // namespace

extern "C" {

const char *esp_err_to_name(esp_err_t e)
{
    switch (e) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
        case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
        case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
        default: return "ESP_ERR";
    }
}

void mpx_log(char level, const char *tag, const char *fmt, ...)
{
    char line[LOG_LEN];
    int n = snprintf(line, sizeof line, "%c (%lu) %s: ", level,
                     static_cast<unsigned long>(esp_log_timestamp()), tag ? tag : "");
    if (n < 0) n = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - n, fmt, ap);
    va_end(ap);
    Guard g(s_log_lock);
    if (s_log_count == LOG_LINES) {           /* full: drop the oldest */
        s_log_head = (s_log_head + 1) % LOG_LINES;
        --s_log_count;
        ++s_log_dropped;
    }
    const int slot = (s_log_head + s_log_count) % LOG_LINES;
    memcpy(s_log[slot], line, LOG_LEN);
    s_log[slot][LOG_LEN - 1] = 0;
    ++s_log_count;
}

bool mpx_log_take(char *out, size_t n)
{
    Guard g(s_log_lock);
    if (!s_log_count) return false;
    strncpy(out, s_log[s_log_head], n - 1);
    out[n - 1] = 0;
    s_log_head = (s_log_head + 1) % LOG_LINES;
    --s_log_count;
    return true;
}

/* ── time ──────────────────────────────────────────────────────────────── */
#if defined(__ZEPHYR__)
int64_t esp_timer_get_time(void)
{
    /* micros() is the 32-bit cycle-counter clock; extend it to 64 bits */
    static uint32_t last = 0, high = 0;
    mpx_critical_enter();
    const uint32_t now = micros();
    if (now < last) ++high;
    last = now;
    const int64_t t = (static_cast<int64_t>(high) << 32) | now;
    mpx_critical_exit();
    return t;
}
void     esp_rom_delay_us(uint32_t us) { delayMicroseconds(us); }
uint32_t esp_cpu_get_cycle_count(void) { return k_cycle_get_32(); }
void     vTaskDelay(TickType_t ticks) { if (ticks) k_msleep(static_cast<int32_t>(ticks)); else k_yield(); }
void     mpx_critical_enter(void) { k_sched_lock(); }
void     mpx_critical_exit(void) { k_sched_unlock(); }
#else
static const auto s_t0 = std::chrono::steady_clock::now();
int64_t esp_timer_get_time(void)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - s_t0).count();
}
void     esp_rom_delay_us(uint32_t us) { const int64_t end = esp_timer_get_time() + us; while (esp_timer_get_time() < end) {} }
uint32_t esp_cpu_get_cycle_count(void) { return static_cast<uint32_t>(esp_timer_get_time() * 160); }
void     vTaskDelay(TickType_t ticks) { std::this_thread::sleep_for(std::chrono::milliseconds(ticks ? ticks : 0)); if (!ticks) std::this_thread::yield(); }
static std::recursive_mutex s_crit;
void     mpx_critical_enter(void) { s_crit.lock(); }
void     mpx_critical_exit(void) { s_crit.unlock(); }
#endif
uint32_t esp_log_timestamp(void) { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
int      esp_cpu_get_core_id(void) { return 1; }
uint32_t esp_rom_get_cpu_ticks_per_us(void) { return 160; }

/* ── tasks ─────────────────────────────────────────────────────────────── */
TickType_t xTaskGetTickCount(void) { return static_cast<TickType_t>(esp_timer_get_time() / 1000); }

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack,
                                   void *arg, UBaseType_t prio, TaskHandle_t *out, BaseType_t core)
{
    (void)core;
    mpx_task *t = nullptr;
    mpx_critical_enter();
    for (auto &x : s_tasks) if (!x.used) { t = &x; t->used = true; break; }
    mpx_critical_exit();
    if (!t) { mpx_log('E', "shim", "no task slot for %s", name); return pdFAIL; }
    t->fn = fn;
    t->arg = arg;
    strncpy(t->name, name ? name : "task", sizeof t->name - 1);
    t->name[sizeof t->name - 1] = 0;
#if defined(__ZEPHYR__)
    /* The ESP counts the stack in bytes too. Zephyr's float context and the
     * printf of floats want some more; the stack comes from the C heap (all
     * the RAM the sketch leaves), not the kernel's 32 KB pool. */
    stack += 1024;
    if (stack < 4096) stack = 4096;
    stack = (stack + 63u) & ~63u;
    void *mem = malloc(stack + 256);
    if (!mem) { t->used = false; mpx_log('E', "shim", "no %lu B stack for %s", (unsigned long)stack, name); return pdFAIL; }
    t->stack_mem = mem;
    k_thread_stack_t *st = reinterpret_cast<k_thread_stack_t *>((reinterpret_cast<uintptr_t>(mem) + 63u) & ~uintptr_t(63));
    k_sem_init(&t->notify, 0, 0xffff);
    k_tid_t tid = k_thread_create(&t->thread, st, stack, task_entry, t, nullptr, nullptr,
                                  zephyr_prio(prio), 0, K_NO_WAIT);
    k_thread_name_set(tid, t->name);
#else
    (void)stack; (void)prio;
    std::mutex started_mu;
    std::condition_variable started_cv;
    bool started = false;
    std::thread th([t, &started_mu, &started_cv, &started] {
        { std::lock_guard<std::mutex> g(started_mu); t->id = std::this_thread::get_id(); started = true; }
        started_cv.notify_one();
        t->fn(t->arg);
        t->used = false;
    });
    { std::unique_lock<std::mutex> g(started_mu); started_cv.wait(g, [&] { return started; }); }
    th.detach();
#endif
    if (out) *out = t;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t h)
{
    mpx_task *t = h ? h : task_self();
    if (!t) return;
#if defined(__ZEPHYR__)
    const bool self = &t->thread == k_sched_current_thread_query();
    t->used = false;
    if (self) {
        k_thread_abort(k_sched_current_thread_query());          /* the stack stays allocated: a thread cannot free its own */
    } else {
        k_thread_abort(&t->thread);
        free(t->stack_mem);
    }
#else
    const bool self = t->id == std::this_thread::get_id();
    t->used = false;
    if (self) pthread_exit(nullptr);
#endif
}

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return task_self(); }

void xTaskNotifyGive(TaskHandle_t h)
{
    if (!h) return;
#if defined(__ZEPHYR__)
    k_sem_give(&h->notify);
#else
    { std::lock_guard<std::mutex> g(h->mu); ++h->count; }
    h->cv.notify_all();
#endif
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
    mpx_task *t = task_self();
    if (!t) { vTaskDelay(ticks == portMAX_DELAY ? 1000 : ticks); return 0; }
#if defined(__ZEPHYR__)
    if (k_sem_take(&t->notify, ticks == portMAX_DELAY ? K_FOREVER : K_MSEC(ticks)) != 0) return 0;
    uint32_t n = 1;
    if (clear) { n += k_sem_count_get(&t->notify); k_sem_reset(&t->notify); }
    return n;
#else
    std::unique_lock<std::mutex> g(t->mu);
    auto ready = [t] { return t->count > 0; };
    if (ticks == portMAX_DELAY) t->cv.wait(g, ready);
    else if (!t->cv.wait_for(g, std::chrono::milliseconds(ticks), ready)) return 0;
    uint32_t n = clear ? t->count : 1;
    t->count -= n;
    return n;
#endif
}

}  // extern "C"

/* ── semaphores and queues ─────────────────────────────────────────────── */
struct mpx_sem {
    bool binary;
#if defined(__ZEPHYR__)
    struct k_mutex m;
    struct k_sem s;
#else
    std::recursive_timed_mutex m;
    std::mutex sm;
    std::condition_variable cv;
    bool given = false;
#endif
};

struct mpx_queue {
#if defined(__ZEPHYR__)
    struct k_msgq q;
    char *buf;
#else
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> items;
    size_t len, size;
#endif
};

extern "C" {

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    mpx_sem *m = new mpx_sem();
    m->binary = false;
#if defined(__ZEPHYR__)
    k_mutex_init(&m->m);              /* Zephyr's mutex is recursive for its owner */
#endif
    return m;
}
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return xSemaphoreCreateMutex(); }

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    mpx_sem *m = new mpx_sem();
    m->binary = true;
#if defined(__ZEPHYR__)
    k_sem_init(&m->s, 0, 1);          /* FreeRTOS: created empty */
#endif
    return m;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t ticks)
{
    if (!m) return pdFALSE;
#if defined(__ZEPHYR__)
    const k_timeout_t to = ticks == portMAX_DELAY ? K_FOREVER : (ticks ? K_MSEC(ticks) : K_NO_WAIT);
    return (m->binary ? k_sem_take(&m->s, to) : k_mutex_lock(&m->m, to)) == 0 ? pdTRUE : pdFALSE;
#else
    if (!m->binary) {
        if (ticks == portMAX_DELAY) { m->m.lock(); return pdTRUE; }
        return m->m.try_lock_for(std::chrono::milliseconds(ticks)) ? pdTRUE : pdFALSE;
    }
    std::unique_lock<std::mutex> g(m->sm);
    auto ready = [m] { return m->given; };
    if (ticks == portMAX_DELAY) m->cv.wait(g, ready);
    else if (!m->cv.wait_for(g, std::chrono::milliseconds(ticks), ready)) return pdFALSE;
    m->given = false;
    return pdTRUE;
#endif
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t m)
{
    if (!m) return pdFALSE;
#if defined(__ZEPHYR__)
    if (m->binary) { k_sem_give(&m->s); return pdTRUE; }
    return k_mutex_unlock(&m->m) == 0 ? pdTRUE : pdFALSE;
#else
    if (!m->binary) { m->m.unlock(); return pdTRUE; }
    { std::lock_guard<std::mutex> g(m->sm); m->given = true; }
    m->cv.notify_one();
    return pdTRUE;
#endif
}

void vSemaphoreDelete(SemaphoreHandle_t m) { delete m; }

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t size)
{
    mpx_queue *q = new mpx_queue();
#if defined(__ZEPHYR__)
    const size_t isz = (size + 3u) & ~3u;
    q->buf = static_cast<char *>(malloc(isz * len));
    if (!q->buf) { delete q; return nullptr; }
    k_msgq_init(&q->q, q->buf, isz, len);
#else
    q->len = len; q->size = size;
#endif
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
#if defined(__ZEPHYR__)
    /* k_msgq copies its own item size: pad through a local copy */
    char tmp[128];
    const size_t isz = q->q.msg_size;
    if (isz > sizeof tmp) return pdFALSE;
    memcpy(tmp, item, isz);
    return k_msgq_put(&q->q, tmp, ticks == portMAX_DELAY ? K_FOREVER : K_MSEC(ticks)) == 0 ? pdTRUE : pdFALSE;
#else
    std::unique_lock<std::mutex> g(q->mu);
    if (q->items.size() >= q->len) {
        if (!ticks || !q->cv.wait_for(g, std::chrono::milliseconds(ticks == portMAX_DELAY ? 100000000 : ticks),
                                      [q] { return q->items.size() < q->len; })) return pdFALSE;
    }
    q->items.emplace_back(static_cast<const uint8_t *>(item), static_cast<const uint8_t *>(item) + q->size);
    q->cv.notify_all();
    return pdTRUE;
#endif
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks)
{
#if defined(__ZEPHYR__)
    char tmp[128];
    if (k_msgq_get(&q->q, tmp, ticks == portMAX_DELAY ? K_FOREVER : K_MSEC(ticks)) != 0) return pdFALSE;
    memcpy(item, tmp, q->q.msg_size);
    return pdTRUE;
#else
    std::unique_lock<std::mutex> g(q->mu);
    if (q->items.empty()) {
        if (!ticks || !q->cv.wait_for(g, std::chrono::milliseconds(ticks == portMAX_DELAY ? 100000000 : ticks),
                                      [q] { return !q->items.empty(); })) return pdFALSE;
    }
    memcpy(item, q->items.front().data(), q->size);
    q->items.pop_front();
    q->cv.notify_all();
    return pdTRUE;
#endif
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q)
{
#if defined(__ZEPHYR__)
    return k_msgq_num_used_get(&q->q);
#else
    std::lock_guard<std::mutex> g(q->mu);
    return q->items.size();
#endif
}

void vQueueDelete(QueueHandle_t q)
{
#if defined(__ZEPHYR__)
    free(q->buf);
#endif
    delete q;
}

}  // extern "C"

/* ── esp_timer: a list served by one thread ────────────────────────────── */
struct mpx_timer {
    esp_timer_cb_t cb;
    void *arg;
    int64_t due_us;        /* 0: stopped */
    int64_t period_us;     /* 0: once */
    bool skip;
    mpx_timer *next;
};
namespace {
mpx_timer *s_timers = nullptr;
bool s_timer_thread = false;
#if defined(__ZEPHYR__)
struct k_sem s_timer_wake;
#else
std::mutex s_twake_mu;
std::condition_variable s_twake_cv;
bool s_twake = false;
#endif

void timer_wake()
{
#if defined(__ZEPHYR__)
    k_sem_give(&s_timer_wake);
#else
    { std::lock_guard<std::mutex> g(s_twake_mu); s_twake = true; }
    s_twake_cv.notify_all();
#endif
}

void timer_thread(void *)
{
    for (;;) {
        int64_t now = esp_timer_get_time(), next = INT64_MAX;
        esp_timer_cb_t cb = nullptr;
        void *arg = nullptr;
        mpx_critical_enter();
        for (mpx_timer *t = s_timers; t; t = t->next) {
            if (!t->due_us) continue;
            if (t->due_us <= now && !cb) {
                cb = t->cb; arg = t->arg;
                if (t->period_us) {
                    t->due_us += t->period_us;
                    if (t->due_us <= now) t->due_us = t->skip ? now + t->period_us : now;   /* late */
                } else {
                    t->due_us = 0;
                }
            }
            if (t->due_us && t->due_us < next) next = t->due_us;
        }
        mpx_critical_exit();
        if (cb) { cb(arg); continue; }
        const int64_t wait_us = next == INT64_MAX ? 1000000 : next - now;
#if defined(__ZEPHYR__)
        k_sem_take(&s_timer_wake, K_USEC(wait_us > 0 ? wait_us : 1));
#else
        std::unique_lock<std::mutex> g(s_twake_mu);
        s_twake_cv.wait_for(g, std::chrono::microseconds(wait_us > 0 ? wait_us : 1), [] { return s_twake; });
        s_twake = false;
#endif
    }
}
}  // namespace

extern "C" {

esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *out)
{
    if (!s_timer_thread) {
#if defined(__ZEPHYR__)
        k_sem_init(&s_timer_wake, 0, 1);
#endif
        /* ESP priority 22 -> Zephyr 4, with the gait: a 200 Hz policy tick must not wait for HTTP */
        if (xTaskCreatePinnedToCore(timer_thread, "esp_timer", 4096, nullptr, 22, nullptr, 0) != pdPASS)
            return ESP_ERR_NO_MEM;
        s_timer_thread = true;
    }
    mpx_timer *t = new mpx_timer{ a->callback, a->arg, 0, 0, a->skip_unhandled_events, nullptr };
    mpx_critical_enter();
    t->next = s_timers;
    s_timers = t;
    mpx_critical_exit();
    *out = t;
    return ESP_OK;
}

static esp_err_t timer_arm(esp_timer_handle_t t, uint64_t us, bool periodic)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    mpx_critical_enter();
    t->period_us = periodic ? static_cast<int64_t>(us) : 0;
    t->due_us = esp_timer_get_time() + static_cast<int64_t>(us);
    if (!t->due_us) t->due_us = 1;
    mpx_critical_exit();
    timer_wake();
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us)     { return t && t->due_us ? ESP_ERR_INVALID_STATE : timer_arm(t, us, false); }
esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t us) { return t && t->due_us ? ESP_ERR_INVALID_STATE : timer_arm(t, us, true); }
esp_err_t esp_timer_restart(esp_timer_handle_t t, uint64_t us)        { return timer_arm(t, us, t && t->period_us); }
bool      esp_timer_is_active(esp_timer_handle_t t)                   { return t && t->due_us; }

esp_err_t esp_timer_stop(esp_timer_handle_t t)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    mpx_critical_enter();
    const bool was = t->due_us != 0;
    t->due_us = 0;
    mpx_critical_exit();
    return was ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t esp_timer_delete(esp_timer_handle_t t)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    mpx_critical_enter();
    for (mpx_timer **pp = &s_timers; *pp; pp = &(*pp)->next)
        if (*pp == t) { *pp = t->next; break; }
    mpx_critical_exit();
    delete t;
    return ESP_OK;
}

/* ── heap_caps ─────────────────────────────────────────────────────────── */
namespace {
constexpr uint32_t ALIGN_MAGIC = 0xA11C0DEu;
struct AlignHdr { void *orig; uint32_t magic, check; };
}

void *heap_caps_malloc(size_t n, uint32_t) { return malloc(n ? n : 1); }
void *heap_caps_calloc(size_t n, size_t size, uint32_t) { return calloc(n ? n : 1, size ? size : 1); }
void *heap_caps_realloc(void *p, size_t n, uint32_t) { return realloc(p, n); }

void *heap_caps_aligned_alloc(size_t align, size_t n, uint32_t caps)
{
    if (align <= 8) return heap_caps_malloc(n, caps);
    char *raw = static_cast<char *>(malloc(n + align + sizeof(AlignHdr)));
    if (!raw) return nullptr;
    uintptr_t p = (reinterpret_cast<uintptr_t>(raw) + sizeof(AlignHdr) + align - 1) & ~(uintptr_t)(align - 1);
    AlignHdr *h = reinterpret_cast<AlignHdr *>(p) - 1;
    h->orig = raw;
    h->magic = ALIGN_MAGIC;
    h->check = ~static_cast<uint32_t>(reinterpret_cast<uintptr_t>(raw));
    return reinterpret_cast<void *>(p);
}

void *heap_caps_aligned_calloc(size_t align, size_t n, size_t size, uint32_t caps)
{
    void *p = heap_caps_aligned_alloc(align, n * size, caps);
    if (p) memset(p, 0, n * size);
    return p;
}

void heap_caps_free(void *p)
{
    if (!p) return;
    AlignHdr *h = static_cast<AlignHdr *>(p) - 1;
    if ((reinterpret_cast<uintptr_t>(p) & 15u) == 0 && h->magic == ALIGN_MAGIC &&
        h->check == ~static_cast<uint32_t>(reinterpret_cast<uintptr_t>(h->orig))) {
        h->magic = 0;
        free(h->orig);
        return;
    }
    free(p);
}

size_t heap_caps_get_largest_free_block(uint32_t)
{
#if defined(__ZEPHYR__)
    /* the C library keeps no statistics we can reach: find it by trying */
    size_t lo = 0, hi = 640u * 1024u;
    while (hi - lo > 1024) {
        const size_t mid = (lo + hi) / 2;
        void *p = malloc(mid);
        if (p) { free(p); lo = mid; } else { hi = mid; }
    }
    return lo;
#else
    return 4u * 1024u * 1024u;
#endif
}
size_t heap_caps_get_free_size(uint32_t caps) { return heap_caps_get_largest_free_block(caps); }
size_t heap_caps_get_minimum_free_size(uint32_t caps) { return heap_caps_get_largest_free_block(caps); }
size_t esp_get_free_heap_size(void) { return heap_caps_get_largest_free_block(0); }

/* ── random ────────────────────────────────────────────────────────────── */
uint32_t esp_random(void)
{
    static uint32_t x = 0;
    mpx_critical_enter();
    if (!x) x = esp_cpu_get_cycle_count() | 1u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    const uint32_t r = x ^ esp_cpu_get_cycle_count();
    mpx_critical_exit();
    return r;
}
void esp_fill_random(void *buf, size_t len)
{
    uint8_t *b = static_cast<uint8_t *>(buf);
    for (size_t i = 0; i < len; i += 4) {
        const uint32_t r = esp_random();
        memcpy(b + i, &r, len - i < 4 ? len - i : 4);
    }
}

/* ── NVS ───────────────────────────────────────────────────────────────── */
/* handles: 1 + the index of the namespace in this table */
static const char *const NVS_NS[] = { "robot", "display", "imu", "policy", "pol", "face", "battery", "system", "voice" };
static const int NVS_NS_N = sizeof NVS_NS / sizeof NVS_NS[0];

static bool nvs_key(nvs_handle_t h, const char *key, char *out, size_t n)
{
    if (h < 1 || h > (nvs_handle_t)NVS_NS_N || !key) return false;
    const int w = h == 1 ? snprintf(out, n, "%s", key) : snprintf(out, n, "%s.%s", NVS_NS[h - 1], key);
    return w > 0 && (size_t)w < n;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)mode;
    /* "parameter" is the reference firmware's, which robot.cpp only reads to
     * import an old calibration: there is none here. */
    for (int i = 0; i < NVS_NS_N; ++i)
        if (strcmp(ns, NVS_NS[i]) == 0) { *out = (nvs_handle_t)(i + 1); return ESP_OK; }
    return ESP_ERR_NVS_NOT_FOUND;
}
void      nvs_close(nvs_handle_t) {}
esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }

static esp_err_t nvs_set_int(nvs_handle_t h, const char *key, int32_t v)
{
    char k[NVS_KEY];
    if (!nvs_key(h, key, k, sizeof k)) return ESP_ERR_INVALID_ARG;
    Guard g(s_nvs_lock);
    return nvs_put(k, 'i', &v, sizeof v, true) ? ESP_OK : ESP_ERR_NO_MEM;
}
static esp_err_t nvs_get_int(nvs_handle_t h, const char *key, int32_t *v)
{
    char k[NVS_KEY];
    if (!nvs_key(h, key, k, sizeof k)) return ESP_ERR_INVALID_ARG;
    Guard g(s_nvs_lock);
    const NvsEntry *e = nvs_find(k);
    if (!e || e->type != 'i' || e->len != sizeof(int32_t)) return ESP_ERR_NVS_NOT_FOUND;
    memcpy(v, e->v, sizeof(int32_t));
    return ESP_OK;
}
esp_err_t nvs_set_i32(nvs_handle_t h, const char *key, int32_t v) { return nvs_set_int(h, key, v); }
esp_err_t nvs_get_i32(nvs_handle_t h, const char *key, int32_t *v) { return nvs_get_int(h, key, v); }
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v) { return nvs_set_int(h, key, (int32_t)v); }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *v) { int32_t x; esp_err_t e = nvs_get_int(h, key, &x); if (!e) *v = (uint32_t)x; return e; }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v)   { return nvs_set_int(h, key, v); }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v)  { int32_t x; esp_err_t e = nvs_get_int(h, key, &x); if (!e) *v = (uint8_t)x; return e; }
esp_err_t nvs_set_i8(nvs_handle_t h, const char *key, int8_t v)    { return nvs_set_int(h, key, v); }
esp_err_t nvs_get_i8(nvs_handle_t h, const char *key, int8_t *v)   { int32_t x; esp_err_t e = nvs_get_int(h, key, &x); if (!e) *v = (int8_t)x; return e; }
esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t v) { return nvs_set_int(h, key, v); }
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *v){ int32_t x; esp_err_t e = nvs_get_int(h, key, &x); if (!e) *v = (uint16_t)x; return e; }

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t n)
{
    char k[NVS_KEY];
    if (!nvs_key(h, key, k, sizeof k)) return ESP_ERR_INVALID_ARG;
    Guard g(s_nvs_lock);
    return nvs_put(k, 'b', v, n, true) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *v, size_t *n)
{
    char k[NVS_KEY];
    if (!nvs_key(h, key, k, sizeof k)) return ESP_ERR_INVALID_ARG;
    Guard g(s_nvs_lock);
    const NvsEntry *e = nvs_find(k);
    if (!e || e->type != 'b') return ESP_ERR_NVS_NOT_FOUND;
    if (v) {
        if (*n < e->len) return ESP_ERR_INVALID_SIZE;
        memcpy(v, e->v, e->len);
    }
    *n = e->len;
    return ESP_OK;
}

/* strings: a blob with its NUL, as the ESP reports the length */
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *v) { return nvs_set_blob(h, key, v, strlen(v) + 1); }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *v, size_t *n) { return nvs_get_blob(h, key, v, n); }

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    char k[NVS_KEY];
    if (!nvs_key(h, key, k, sizeof k)) return ESP_ERR_INVALID_ARG;
    Guard g(s_nvs_lock);
    NvsEntry *e = nvs_find(k);
    if (!e || e->type == 'x') return ESP_ERR_NVS_NOT_FOUND;
    e->type = 'x';                     /* Linux drops it from nvs.json */
    e->len = 0;
    e->dirty = true;
    return ESP_OK;
}

bool mpx_nvs_load(const char *key, uint8_t type, const void *v, size_t n)
{
    Guard g(s_nvs_lock);
    return nvs_put(key, type, v, n, false) != nullptr;
}

bool mpx_nvs_take_dirty(char *key, size_t keylen, uint8_t *type, uint8_t *v, size_t *n)
{
    Guard g(s_nvs_lock);
    for (int i = 0; i < s_nvs_n; ++i) {
        NvsEntry &e = s_nvs[i];
        if (!e.dirty) continue;
        e.dirty = false;
        strncpy(key, e.key, keylen - 1);
        key[keylen - 1] = 0;
        *type = e.type;
        memcpy(v, e.v, e.len);
        *n = e.len;
        return true;
    }
    return false;
}

}  // extern "C"
