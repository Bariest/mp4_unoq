/* policy_remote.cpp -- the RL networks run on Linux (python/policy_net.py):
 * pol_forward() of a file without weights (MPXP_FLAG_REMOTE) comes here, on
 * the runner's infer task, and asks Linux over the Bridge.
 *
 * The firmware-dev policies are 0.13 to 0.77 MB of float weights; the MCU has
 * 0.77 MB of flash for everything. On Linux numpy evaluates the biggest in
 * well under a millisecond; the Bridge (460800 baud) adds a few: inside the
 * 20 ms step that policy_run.cc gives its network (infer_task runs beside the
 * 5 ms ticks).
 *
 * A network with a history (sku1v2_recovery: the last 10 observations) takes
 * 450 floats. Only the newest observation goes over the Bridge: Linux keeps
 * the same history (contract.py HistoryStacker), and this side checks every
 * time that Linux's copy is the one the runner stacked (a shadow of the last
 * input): the step after it (send 1 frame), a fresh start (every frame the
 * same: Linux resets), or anything else - the self-test's fixtures - the
 * whole input in pieces. */
#include <Arduino_RouterBridge.h>
#include <math.h>
#include <string.h>
#include "shim_esp.h"
#include "policy_core.h"

namespace {

const char *TAG = "pol_remote";
using Bin = MsgPack::bin_t<uint8_t>;

enum : int { F_PUSH = 0, F_RESET = 1, F_FULL = 2 };
constexpr int TERM[7] = { 0, 3, 6, 9, 21, 33, 45 };   /* contract.py OBS_TERMS, as pol_hist_stack */
constexpr size_t PIECE = 200;

struct Shadow {
    uint32_t id = 0;
    int len = 0;
    float in[POL_OBS * POL_HIST_MAX];
    bool valid = false;
};
Shadow s_shadow[4];
uint32_t s_seq = 0;

uint32_t net_id(const mpxp_header_t *h)
{
    uint32_t x = 2166136261u;                            /* FNV-1a of the policy's name */
    for (size_t i = 0; i < sizeof h->name && h->name[i]; ++i) { x ^= (uint8_t)h->name[i]; x *= 16777619u; }
    return x;
}

Shadow *shadow_of(uint32_t id, int L)
{
    for (auto &s : s_shadow) if (s.valid && s.id == id) return &s;
    static int next = 0;
    Shadow *s = &s_shadow[next++ % 4];
    s->id = id;
    s->len = L;
    s->valid = false;
    return s;
}

/* frame k (0 the oldest) of a stacked input, as pol_hist_stack laid it out */
void frame_of(const float *in, int L, int k, float out[POL_OBS])
{
    int o = 0;
    for (int t = 0; t < 6; ++t) {
        const int a = TERM[t], n = TERM[t + 1] - a;
        memcpy(out + a, in + o + k * n, sizeof(float) * n);
        o += n * L;
    }
}

Bin floats_bin(const float *f, size_t n)
{
    Bin b;
    b.resize(n * sizeof(float));
    memcpy(b.data(), f, n * sizeof(float));
    return b;
}

}  // namespace

extern "C" void mpx_pol_remote_forward(const pol_net_t *net, const float *obs, float act[POL_ACT])
{
    const mpxp_header_t *h = net->h;
    const int L = pol_history(h);
    const uint32_t id = net_id(h);
    const size_t n_in = (size_t)POL_OBS * L;
    float newest[POL_OBS];
    int flag = F_PUSH;
    Shadow *sh = nullptr;

    if (L <= 1) {
        memcpy(newest, obs, sizeof newest);
    } else {
        sh = shadow_of(id, L);
        frame_of(obs, L, L - 1, newest);
        bool all_same = true, follows = sh->valid && sh->len == L;
        float f[POL_OBS], g[POL_OBS];
        for (int k = 0; k < L && (all_same || follows); ++k) {
            frame_of(obs, L, k, f);
            if (all_same && memcmp(f, newest, sizeof f) != 0) all_same = false;
            if (follows && k < L - 1) {
                frame_of(sh->in, L, k + 1, g);
                if (memcmp(f, g, sizeof f) != 0) follows = false;
            }
        }
        flag = follows ? F_PUSH : all_same ? F_RESET : F_FULL;
        if (flag == F_FULL) {
            for (size_t off = 0; off < n_in * sizeof(float); off += PIECE) {
                const size_t n = n_in * sizeof(float) - off < PIECE ? n_in * sizeof(float) - off : PIECE;
                Bin b(reinterpret_cast<const uint8_t *>(obs) + off, reinterpret_cast<const uint8_t *>(obs) + off + n);
                bool ok = false;
                if (!Bridge.call("pol_obs_put", (int)off, b).result(ok) || !ok) {
                    for (int i = 0; i < POL_ACT; ++i) act[i] = NAN;      /* the runner stops on it */
                    sh->valid = false;
                    return;
                }
            }
        }
    }

    Bin out;
    const Bin in = flag == F_FULL ? Bin() : floats_bin(newest, POL_OBS);
    const bool ok = Bridge.call("pol_fwd", (int)id, (int)++s_seq, flag, in).result(out);
    if (!ok || out.size() != POL_ACT * sizeof(float)) {
        static uint32_t warned = 0;
        if (millis() - warned > 2000) {
            warned = millis();
            ESP_LOGW(TAG, "no answer from Linux for %s (%s, %u bytes): is the app's Python running?", h->name,
                     ok ? "short" : "error", (unsigned)out.size());
        }
        for (int i = 0; i < POL_ACT; ++i) act[i] = NAN;                  /* non-finite: graceful stop */
        if (sh) sh->valid = false;
        return;
    }
    memcpy(act, out.data(), sizeof(float) * POL_ACT);
    if (sh) {
        memcpy(sh->in, obs, n_in * sizeof(float));
        sh->valid = true;
    }
}
