#ifndef SC_CONFIG_H
#define SC_CONFIG_H

typedef struct {
    int   assist_enabled;
    int   passthrough;
    int   grab;
    int   loop_hz;
    int   hud;
    int   use_filter;        /* simplified settings preset */
    int   graph_selection;   /* 1=none, 2=static, 3=live */

    float filter_setting;    /* 0..1, drives the other sliders when use_filter */
    float steering_rate;
    float rate_increase_with_speed;
    float target_slip_deg;
    float target_slip_scale;
    float self_steer_response;
    float damping_strength;
    float max_self_steer_angle;
    float countersteer_response;
    float max_dynamic_limit_reduction;
    float steering_lock_deg;
    float wheelbase_m;
    float stick_gamma;
    float deadzone;

    float steer_sign; /* +1 or -1 */
    float yaw_sign;
    float lat_sign;
    float fwd_sign;   /* +1 = +Z forward (AC), -1 = -Z forward (AMS2) */
    int   swap_xz;

    int   invert_throttle;
    int   invert_brake;

    char  gamepad_name[128];
    char  shm_path[256];
    int   udp_port;
    char  path[512];
} ScConfig;

void sc_config_defaults(ScConfig *c);
int  sc_config_load(ScConfig *c, const char *path);
int  sc_config_reload_if_changed(ScConfig *c);
int  sc_config_save(const ScConfig *c);
void sc_config_apply_filter(ScConfig *c);
void sc_config_touch_mtime(const ScConfig *c);

#endif
