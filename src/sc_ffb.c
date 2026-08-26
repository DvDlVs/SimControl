#include "sc_ffb.h"

#include <math.h>
#include <string.h>

#ifndef M_PI_F
#define M_PI_F 3.14159265358979f
#endif

static float clamp01f(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

static struct {
    float rad_est;      /* estimated tyre radius (m) */
    float last_v;
    int   have_v;
    float vy_hp;        /* high-passed vertical velocity */
    float ax_lp;        /* low-passed longitudinal accel (proxy path) */
    float road_pulse;
    double road_until;
    int   road_armed;
    double clock;         /* module time, advanced by dt */
    float lock_env, weak_env;
} ffb = { .rad_est = 0.31f };

/* envelope: fast attack, slower release */
static float env(float cur, float target, double dt)
{
    float rate = target > cur ? 14.f : 11.f;
    cur += (target - cur) * fminf(1.f, (float)dt * rate);
    if (cur < 0.0015f) cur = 0.f;
    return cur;
}

void sc_ffb_update(ScInput *in, const ScTelem *tm, const ScPadState *pad,
                   const ScConfig *cfg, double dt)
{
    float strong_t = 0.f, weak_road_t = 0.f;

    if (!cfg->ffb_enabled || !in || !tm || !pad || !tm->connected || !tm->playing)
        goto emit;

    float v = tm->speed > 0.f ? tm->speed : 0.f;
    float throttle = clamp01f(pad->throttle);
    float brake = clamp01f(pad->brake);
    float dtf = dt > 0.0001f ? (float)dt : 0.004f;

    /* ---- wheel lock / spin -------------------------------------- */
    if (tm->wheel_valid && v > 3.f) {
        float rps[4];
        float sum = 0.f;
        for (int i = 0; i < 4; i++) {
            rps[i] = fabsf(tm->wheel_rps[i]);
            sum += rps[i];
        }
        float avg = sum * 0.25f;
        /* adapt radius estimate only in steady rolling */
        if (avg > 2.f && fabsf(throttle - brake) < 0.15f && v > 8.f) {
            float r_now = v / (avg * 2.f * M_PI_F);
            r_now = clamp01f((r_now - 0.20f) / 0.30f) * 0.30f + 0.20f;
            ffb.rad_est += (r_now - ffb.rad_est) * fminf(1.f, dtf * 0.8f);
        }
        float vmin = 1e9f;
        for (int i = 0; i < 4; i++) {
            float lin = rps[i] * 2.f * M_PI_F * ffb.rad_est;
            if (lin < vmin) vmin = lin;
        }
        if (brake > 0.12f) {
            float deficit = (v - vmin) / fmaxf(v, 6.f);       /* 0..~1 */
            float lock = clamp01f((deficit - 0.12f) / 0.40f);
            strong_t = lock * brake * cfg->ffb_lock_gain;
        }
    } else if (v > 2.f && ffb.have_v) {
        /* proxies for games without per-wheel rotation.
         * Longitudinal acceleration is LOW-PASSED first: a sports car
         * legitimately pulls 4-6 m/s^2 on throttle, so the raw spike
         * fired constantly. Sustained traction loss exceeds ~5.5 m/s^2;
         * lock-up deceleration exceeds ~8 m/s^2. */
        float ax = (v - ffb.last_v) / dtf;
        if (fabsf(ax) < 25.f)
            ffb.ax_lp += (ax - ffb.ax_lp) * fminf(1.f, dtf * 6.f);
        if (brake > 0.18f) {
            float lock = clamp01f((-ffb.ax_lp - 8.0f) / 4.0f);
            strong_t = lock * brake * cfg->ffb_lock_gain;
        }

    }
    ffb.last_v = v;
    ffb.have_v = 1;

    /* ---- road texture: high-passed vertical velocity -------------- */
    {
        float vy = tm->local_vy;
        ffb.vy_hp += (vy - ffb.vy_hp) * fminf(1.f, dtf * 9.f);   /* ~1.4 Hz cut */
        float hp = fabsf(vy - ffb.vy_hp);
        /* kerbs / impacts: EDGE-TRIGGERED pulse on the high-passed body
         * vertical velocity. A level-based gate chatters around the noise
         * floor (felt as endless buzz); a rising-edge pulse with a fixed
         * duration and a re-arm requirement cannot stay stuck on. */
        if (!ffb.road_armed) {
            if (hp < 0.45f) ffb.road_armed = 1;
        } else if (hp > 0.85f) {
            ffb.road_pulse = clamp01f((hp - 0.85f) / 1.2f) *
                             1.9f * cfg->ffb_road_gain;
            ffb.road_until = ffb.clock + 0.13;
            ffb.road_armed = 0;
        }
        weak_road_t = (ffb.clock < ffb.road_until) ? ffb.road_pulse : 0.f;
    }

emit:
    /* Channel map (user preference):
     *   BIG motor   <- kerbs / impacts   (edge pulses)
     *   SMALL motor <- wheel lock        (strong_t)  */
    ffb.clock += dt;
    ffb.lock_env = env(ffb.lock_env, clamp01f(strong_t), dt);
    ffb.weak_env = env(ffb.weak_env, clamp01f(weak_road_t), dt);

    sc_input_rumble(in, ffb.weak_env, ffb.lock_env);
}

void sc_ffb_levels(float *strong01, float *weak01)
{
    if (strong01) *strong01 = ffb.lock_env;
    if (weak01) *weak01 = ffb.weak_env;
}
