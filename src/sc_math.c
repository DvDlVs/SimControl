#include "sc_math.h"

float sc_clamp_eased(float val, float min_val, float max_val, float easing_window) {
    float span = max_val - min_val;
    if (span <= 1e-12f) return min_val;
    easing_window = sc_clamp01(easing_window);
    float window_scaled = easing_window * span;
    if (window_scaled < 1e-12f) {
        return sc_clampf(val, min_val, max_val);
    }
    float half = window_scaled * 0.5f;
    float min_low  = min_val - half;
    float min_high = min_val + half;
    float max_low  = max_val - half;
    float max_high = max_val + half;

    if (val < min_low)  return min_val;
    if (val > max_high) return max_val;

    if (val < min_high) {
        float t = (val - min_low) / window_scaled;
        return min_val + t * t * half;
    }
    if (val > max_low) {
        float t = (val - max_low - window_scaled) / window_scaled;
        return max_val - t * t * half;
    }
    return val;
}

float sc_inverse_lerp_clamped_eased(float from, float to, float val,
                                     float out_min, float out_max, float window) {
    return sc_clamp_eased(sc_inverse_lerp(from, to, val), out_min, out_max, window);
}

void sc_smooth_init(ScSmooth *s, float rate, float linearity,
                     float min_v, float max_v, float start) {
    s->rate = rate;
    s->linearity = linearity;
    s->range = max_v - min_v;
    if (fabsf(s->range) < 1e-12f) s->range = 1.f;
    s->state = start;
    s->starting = start;
}

float sc_smooth_get(ScSmooth *s, float val, float dt) {
    float lin_sq = s->linearity * s->linearity;
    float denom = 1.f - (1.f / (lin_sq + (1.f / 0.75f)));
    if (fabsf(denom) < 1e-8f) denom = 1e-8f;
    float rate = s->rate / denom * 0.5f;
    float diff_abs_n = fabsf((val - s->state) / s->range);
    float diff_sign = sc_signf(val - s->state);
    float adjusted = (diff_abs_n * (1.f - lin_sq) + lin_sq) * rate;
    float step_n = diff_abs_n;
    float max_step_n = dt * adjusted;
    if (step_n > max_step_n) step_n = max_step_n;
    s->state = s->state + diff_sign * step_n * s->range;
    return s->state;
}

float sc_smooth_get_rate(ScSmooth *s, float val, float dt, float rate) {
    float old = s->rate;
    s->rate = rate;
    float r = sc_smooth_get(s, val, dt);
    s->rate = old;
    return r;
}

float sc_smooth_get_rate_mult(ScSmooth *s, float val, float dt, float mult) {
    return sc_smooth_get_rate(s, val, dt, s->rate * mult);
}
