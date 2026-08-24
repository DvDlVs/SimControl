#ifndef SC_MATH_H
#define SC_MATH_H

#include <math.h>
#include <stdbool.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline float sc_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float sc_clamp01(float v) {
    return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

static inline float sc_clamp1(float v) {
    return v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
}

static inline float sc_signf(float v) {
    return (v > 0.f) ? 1.f : ((v < 0.f) ? -1.f : 0.f);
}

static inline float sc_lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

static inline float sc_inverse_lerp(float from, float to, float value) {
    float dif = to - from;
    return (fabsf(dif) > 1e-12f) ? ((value - from) / dif) : 0.f;
}

static inline float sc_lerp_inv_sat(float x, float a, float b) {
    return sc_clamp01(sc_inverse_lerp(a, b, x));
}

static inline float sc_number_guard(float v, float alt) {
    return (isnan(v) || isinf(v)) ? alt : v;
}

static inline float sc_zero_guard(float v) {
    if (v == 0.f || !isfinite(v)) return 1e-15f;
    float inv = 1.f / v;
    inv = sc_clampf(inv, -1e15f, 1e15f);
    return 1.f / inv;
}

static inline float sc_signed_pow(float x, float y) {
    return sc_signf(x) * powf(fabsf(x), y);
}

static inline float sc_deg(float rad) {
    return rad * (180.f / (float)M_PI);
}

static inline float sc_rad(float deg) {
    return deg * ((float)M_PI / 180.f);
}

float sc_clamp_eased(float val, float min_val, float max_val, float easing_window);
float sc_inverse_lerp_clamped_eased(float from, float to, float val,
                                     float out_min, float out_max, float window);

typedef struct {
    float rate;
    float linearity;
    float range;
    float state;
    float starting;
} ScSmooth;

void sc_smooth_init(ScSmooth *s, float rate, float linearity,
                     float min_v, float max_v, float start);
float sc_smooth_get(ScSmooth *s, float val, float dt);
float sc_smooth_get_rate(ScSmooth *s, float val, float dt, float rate);
float sc_smooth_get_rate_mult(ScSmooth *s, float val, float dt, float mult);

#endif
