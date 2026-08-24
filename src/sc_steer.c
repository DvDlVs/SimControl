#include "sc_steer.h"
#include <string.h>

void sc_steer_reset(ScSteerState *st) {
    memset(st, 0, sizeof(*st));
    sc_smooth_init(&st->steering,     7.0f, 0.13f, -1.f,  1.f,  0.f);
    sc_smooth_init(&st->abs_steering, 7.0f, 0.13f, -1.f,  1.f,  0.f);
    sc_smooth_init(&st->self_steer,   7.0f, 0.13f, -1.f,  1.f,  0.f);
    sc_smooth_init(&st->limit,       11.0f, 0.01f,  0.f, 32.f,  8.f);
    sc_smooth_init(&st->grounded,     4.0f, 1.00f,  0.f,  1.f,  1.f);
    sc_smooth_init(&st->counter,     12.0f, 1.00f,  0.f,  1.f,  0.f);
    sc_smooth_init(&st->front_slip,  10.0f, 0.05f,  0.f,  2.f,  0.f);
    sc_smooth_init(&st->rear_slip,   10.0f, 0.05f,  0.f,  2.f,  0.f);
    st->inited = 1;
}

static float steering_rate_mult(const ScConfig *cfg, float fwd_vel, float lock_deg) {
    float v = fwd_vel > 8.f ? fwd_vel : 8.f;
    float speed_adj = lock_deg / fminf(65.f / (v - 7.3f) + 3.5f, lock_deg);
    if (speed_adj < 1e-6f) speed_adj = 1e-6f;
    return powf(speed_adj, cfg->rate_increase_with_speed) * cfg->steering_rate;
}

void sc_steer_step(ScSteerState *st, const ScConfig *cfg,
                    const ScVehicle *veh, float raw_steer, float dt,
                    ScSteerOut *out) {
    if (!st->inited) sc_steer_reset(st);
    memset(out, 0, sizeof(*out));

    float lock = cfg->steering_lock_deg;
    if (lock < 5.f) lock = 5.f;
    float wb = cfg->wheelbase_m;
    if (wb < 1.f) wb = 1.f;
    float wb_factor = wb / 2.5f;

    float vx = sc_number_guard(veh->local_vx, 0.f);
    float vz = sc_number_guard(veh->local_vz, 0.f);
    float yaw = sc_number_guard(veh->yaw_rate, 0.f);
    float a = wb * 0.5f;
    float fwd = (cfg->fwd_sign < 0.f) ? -1.f : 1.f;
    float vz_fwd = vz * fwd;

    /* Bicycle model: v + ω × r along body forward (vz_fwd). */
    float f_lat  = vx + yaw * a;
    float r_lat  = vx - yaw * a;
    float f_long = vz_fwd;
    float r_long = vz_fwd;

    float f_ang = sc_number_guard(sc_deg(atan2f(f_lat, fabsf(f_long))), 0.f);
    float r_ang = sc_number_guard(sc_deg(atan2f(r_lat, fabsf(r_long))), 0.f);
    float f_hvel = sqrtf(f_lat * f_lat + f_long * f_long);
    float hvel = sqrtf(vx * vx + vz * vz);
    float travel = sc_number_guard(sc_deg(atan2f(vx, vz_fwd)), 0.f);

    float grounded_raw = veh->front_grounded > 0.5f ? 1.f : 0.f;
    float grounded = sc_smooth_get(&st->grounded, grounded_raw, dt);

    float rate_mult = steering_rate_mult(cfg, hvel, lock);
    float centering = 1.f;
    if (rate_mult > 0.f && rate_mult < 0.5f) {
        float stv = st->steering.state;
        if ((fabsf(raw_steer) < fabsf(stv) && sc_signf(raw_steer) == sc_signf(stv)) ||
            (sc_signf(raw_steer) != sc_signf(stv))) {
            centering = (rate_mult * 0.5f + 0.25f) / rate_mult;
        }
    }
    int stick_counter = (sc_signf(raw_steer) != sc_signf(sc_zero_guard(r_lat)))
                        && fabsf(raw_steer) > 0.04f;
    int stick_cross = sc_signf(raw_steer) != sc_signf(st->steering.state)
                      && fabsf(raw_steer) > 0.04f && fabsf(st->steering.state) > 0.04f;
    float rate_boost = (stick_counter || stick_cross) ? 2.2f : 1.f;

    float initial = sc_smooth_get_rate_mult(&st->steering, raw_steer, dt,
                                             rate_mult * centering * rate_boost);
    float abs_initial = sc_smooth_get_rate_mult(&st->abs_steering, fabsf(raw_steer), dt,
                                                 rate_mult * centering * rate_boost);

    float fade = sc_lerp_inv_sat(f_hvel, 2.f * wb_factor, 6.f * wb_factor);
    float tslip = cfg->target_slip_deg * cfg->target_slip_scale;
    if (tslip < 0.1f) tslip = 0.1f;
    float f_nd = sc_smooth_get(&st->front_slip, fabsf(f_ang) / tslip, dt);
    float r_nd = sc_smooth_get(&st->rear_slip, fabsf(r_ang) / tslip, dt);

    if (!cfg->assist_enabled || cfg->passthrough || !veh->valid) {
        out->initial = initial;
        out->abs_initial = abs_initial;
        out->output = sc_clamp1(initial);
        out->fade = 0.f;
        out->f_ang_deg = f_ang;
        out->r_ang_deg = r_ang;
        out->travel_deg = travel;
        out->limit = 1.f;
        out->front_nd_slip = f_nd;
        out->rear_nd_slip = r_nd;
        return;
    }

    float input_sign = sc_signf(initial);
    float mid_fade = sc_lerp_inv_sat(hvel, 10.f * wb_factor, 20.f * wb_factor);

    float resp = sc_clampf(cfg->self_steer_response, 0.f, 1.f);
    float corr_exp = 1.f + (1.f - log10f(10.f * (resp * 0.9f + 0.1f)));
    float corr_base = sc_signed_pow(sc_clampf(-r_ang / 72.f, -1.f, 1.f), corr_exp) * 72.f / lock;
    float cap = sc_clamp01(cfg->max_self_steer_angle / lock);
    float strength = fmaxf(1.f, grounded) * fade;
    float damp = yaw * cfg->damping_strength * 0.15f * (30.f / lock);
    float cap_t = cap > 1e-6f ? fminf(1.f, 4.f / (2.f * cap)) : 1.f;
    float raw_ss = sc_clamp_eased(corr_base, -cap, cap, cap_t) + damp;
    float ss = sc_clampf(sc_smooth_get(&st->self_steer, raw_ss, dt), -2.f, 2.f) * strength;

    float target_slip = cfg->target_slip_deg * cfg->target_slip_scale;
    float max_red = sc_lerpf(target_slip * 0.4f, target_slip * 0.75f,
                              sc_clamp01(cfg->max_dynamic_limit_reduction / 10.f));
    float angle_sub = sc_lerpf(max_red, max_red * 0.9f, sc_clamp01(veh->brake));
    float ease_w = (angle_sub * 0.4f) / (lock + 15.f + angle_sub);
    float clamped_f = sc_clamp_eased(input_sign * f_ang, -lock - 15.f, angle_sub, ease_w);

    int is_counter = (input_sign != sc_signf(sc_zero_guard(r_lat))) && fabsf(initial) > 1e-6f;
    float raw_ci = 0.f;
    if (is_counter) {
        raw_ci = sc_inverse_lerp_clamped_eased(4.5f, 10.f, fabsf(r_ang), 0.f, 1.f, 0.6f);
    }
    float ret_rate = sc_lerpf(0.8f, 0.3f, sc_lerp_inv_sat(fabsf(f_ang - r_ang), 2.f, 10.f));
    float ci_mult = (raw_ci < st->counter.state) ? ret_rate : 1.f;
    float counter = sc_smooth_get_rate_mult(&st->counter, raw_ci, dt, ci_mult) * mid_fade;

    float anti_ss = abs_initial * -ss;
    float target_in = target_slip - clamped_f;
    float cm = sc_lerpf(sc_lerp_inv_sat(-input_sign * r_ang, 0.f, 30.f) * (1.f / 3.f) + (2.f / 3.f),
                         1.f, cfg->countersteer_response);
    float target_c = (target_slip * (cfg->countersteer_response * cm * 0.7f + 0.1f))
                     - (input_sign * r_ang);

    float tgt_in_c = sc_clampf(target_in, 0.f, lock);
    float tgt_c_c  = sc_clampf(target_c, 0.f, lock);
    float tgt_ang  = sc_lerpf(tgt_in_c, tgt_c_c, counter);

    float nf = sinf(sc_clampf(sc_rad(travel * 2.f / 3.f), -(float)M_PI * 0.5f, (float)M_PI * 0.5f));
    nf = nf * nf;
    nf = nf * nf;
    nf = nf * nf;
    nf = nf * nf;

    float lim_rate = (tgt_ang > st->limit.state) ? 20.f : 11.f;
    float smooth_lim = sc_smooth_get_rate(&st->limit, tgt_ang, dt, lim_rate);
    float limit = sc_lerpf(smooth_lim / lock, 1.f, nf);

    float processed = sc_clamp1(initial * limit + ss + anti_ss);
    float desired = sc_lerpf(initial, processed, fade);

    out->initial = initial;
    out->abs_initial = abs_initial;
    out->output = sc_clamp1(desired);
    out->fade = fade;
    out->f_ang_deg = f_ang;
    out->r_ang_deg = r_ang;
    out->limit = limit;
    out->self_steer = ss;
    out->travel_deg = travel;
    out->self_steer_strength = strength * (1.f - abs_initial);
    out->max_limit_reduction = max_red;
    out->limit_reduction = fmaxf(clamped_f, 0.f);
    out->front_nd_slip = f_nd;
    out->rear_nd_slip = r_nd;
}
