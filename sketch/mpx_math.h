/* mpx_math.h -- the float maths the ported ESP code needs, without relying on
 * the libm functions a UNO Q sketch may not be able to link.
 *
 * The UNO Q's zephyr core 0.51 loads sketches as extensions and only links the
 * libm functions it exports; sinf and cosf are not among them there (acosf,
 * asinf, atan2f, sqrtf are - the kinematics have always linked against those).
 * Everything here is self-contained. tools/port_from_esp.py rewrites
 * std::sin / std::cos / std::lrintf / std::lround / std::fmin / std::fmax in
 * the ported files to the mpx_ versions below; the C names are macros. */
#pragma once
#include <math.h>
#include <stdint.h>

static inline void mpx_sincosf(float x, float *s, float *c)
{
    /* x = k * pi/2 + r, |r| <= pi/4 (Cody-Waite, two constants) */
    const float two_over_pi = 0.636619772367581343f;
    const float pio2_hi = 1.57079601287841796875f;
    const float pio2_lo = 3.13916473702585697e-07f;
    float kf = x * two_over_pi;
    kf = kf >= 0.0f ? (float)(int32_t)(kf + 0.5f) : (float)(int32_t)(kf - 0.5f);
    const int32_t k = (int32_t)kf;
    const float r = (x - kf * pio2_hi) - kf * pio2_lo;
    const float r2 = r * r;
    /* minimax polynomials on [-pi/4, pi/4] */
    const float sr = r + r * r2 * (-1.6666654611e-1f + r2 * (8.3321608736e-3f + r2 * -1.9515295891e-4f));
    const float cr = 1.0f + r2 * (-0.5f + r2 * (4.166664568298827e-2f + r2 * (-1.388731625493765e-3f + r2 * 2.443315711809948e-5f)));
    switch (k & 3) {
        case 0:  *s = sr;  *c = cr;  break;
        case 1:  *s = cr;  *c = -sr; break;
        case 2:  *s = -sr; *c = -cr; break;
        default: *s = -cr; *c = sr;  break;
    }
}
static inline float mpx_sinf(float x) { float s, c; mpx_sincosf(x, &s, &c); return s; }
static inline float mpx_cosf(float x) { float s, c; mpx_sincosf(x, &s, &c); return c; }

static inline float mpx_hypotf(float a, float b) { return sqrtf(a * a + b * b); }

/* round half away from zero, like lround / lrintf in the default rounding mode
 * for every value the ported code passes (it never relies on ties-to-even) */
static inline long mpx_lrintf(float x) { return (long)(x >= 0.0f ? x + 0.5f : x - 0.5f); }
static inline long mpx_lroundf(float x) { return (long)(x >= 0.0f ? x + 0.5f : x - 0.5f); }

/* e^x: 2^(x*log2 e) = 2^i * 2^f, f in [-0.5, 0.5] by a degree-6 polynomial (~1e-7 rel) */
static inline float mpx_expf(float x)
{
    if (x > 88.0f) return 3.4e38f;
    if (x < -87.0f) return 0.0f;
    const float t = x * 1.44269504088896341f;
    const float fi = t >= 0.0f ? (float)(int32_t)(t + 0.5f) : (float)(int32_t)(t - 0.5f);
    const float f = t - fi;
    const float p = 1.0f + f * (0.693147182f + f * (0.240226507f + f * (0.0555041087f + f * (0.00961812911f
                    + f * (0.00133335581f + f * 0.000154035304f)))));
    union { float f; int32_t i; } u;
    u.f = p;
    u.i += (int32_t)fi << 23;
    return u.f;
}

#define sinf   mpx_sinf
#define cosf   mpx_cosf
#define hypotf mpx_hypotf
#define expf   mpx_expf
#define lroundf mpx_lroundf
#define lrintf  mpx_lrintf

static inline float mpx_expm1f(float x)
{
    if (x > -1e-3f && x < 1e-3f) return x + 0.5f * x * x + x * x * x * (1.0f / 6.0f);   /* no cancellation near 0 */
    return mpx_expf(x) - 1.0f;
}
#define expm1f mpx_expm1f

#ifndef __cplusplus
/* the double versions the C files call (policy_core.c): float precision is what the policy needs */
static inline double mpx_sin_d(double x)  { return (double)mpx_sinf((float)x); }
static inline double mpx_cos_d(double x)  { return (double)mpx_cosf((float)x); }
static inline double mpx_acos_d(double x) { return (double)acosf((float)x); }
static inline double mpx_fmax_d(double a, double b) { return (a < b) ? b : a; }
static inline double mpx_fmin_d(double a, double b) { return (b < a) ? b : a; }
#define sin(x)     mpx_sin_d(x)
#define cos(x)     mpx_cos_d(x)
#define acos(x)    mpx_acos_d(x)
#define fmax(a, b) mpx_fmax_d((a), (b))
#define fmin(a, b) mpx_fmin_d((a), (b))
#endif

#ifdef __cplusplus
static inline float  mpx_sin(float x)  { return mpx_sinf(x); }
static inline double mpx_sin(double x) { return (double)mpx_sinf((float)x); }
static inline float  mpx_cos(float x)  { return mpx_cosf(x); }
static inline double mpx_cos(double x) { return (double)mpx_cosf((float)x); }
static inline long   mpx_lround(float x)  { return mpx_lroundf(x); }
static inline long   mpx_lround(double x) { return (long)(x >= 0.0 ? x + 0.5 : x - 0.5); }
static inline float  mpx_fmin(float a, float b) { return (b < a) ? b : a; }
static inline float  mpx_fmax(float a, float b) { return (a < b) ? b : a; }
static inline double mpx_fmin(double a, double b) { return (b < a) ? b : a; }
static inline double mpx_fmax(double a, double b) { return (a < b) ? b : a; }
#endif
