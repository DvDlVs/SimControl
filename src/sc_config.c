#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sc_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static time_t g_mtime;

void sc_config_defaults(ScConfig *c) {
    memset(c, 0, sizeof(*c));
    c->assist_enabled = 1;
    c->passthrough = 0;
    c->grab = 1;
    c->loop_hz = 250;
    c->hud = 1;
    c->use_filter = 0;
    c->graph_selection = 1;
    c->filter_setting = 0.5f;

    c->steering_rate = 0.55f;
    c->rate_increase_with_speed = 0.0f;
    c->target_slip_deg = 7.0f;
    c->target_slip_scale = 0.95f;
    c->self_steer_response = 0.37f;
    c->damping_strength = 0.37f;
    c->max_self_steer_angle = 90.0f;
    c->countersteer_response = 0.45f;
    c->max_dynamic_limit_reduction = 5.0f;
    c->steering_lock_deg = 20.0f;
    c->wheelbase_m = 2.60f;
    c->stick_gamma = 1.40f;
    c->deadzone = 0.12f;

    c->steer_sign = 1.f;
    c->yaw_sign = 1.f;
    c->lat_sign = 1.f;
    c->fwd_sign = -1.f; /* AMS2 local Z is typically rearward */
    c->swap_xz = 0;
    c->invert_throttle = 0;
    c->ffb_enabled = 1;
    c->ffb_road_cont = 1;
    c->ffb_lock_gain = 0.75f;
    c->ffb_spin_gain = 0.65f;
    c->ffb_road_gain = 0.45f;
    c->invert_brake = 0;

    c->gamepad_name[0] = 0;
    c->shm_path[0] = 0;
    c->udp_port = 5606;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) { *e = 0; e--; }
    return s;
}

static void set_str(char *dst, size_t n, const char *src) {
    snprintf(dst, n, "%s", src);
}

static int parse_line(ScConfig *c, char *line) {
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    line = trim(line);
    if (!*line) return 0;
    char *eq = strchr(line, '=');
    if (!eq) return 0;
    *eq = 0;
    char *key = trim(line);
    char *val = trim(eq + 1);

    #define F(name, field) if (strcmp(key, name) == 0) { c->field = strtof(val, NULL); return 1; }
    #define I(name, field) if (strcmp(key, name) == 0) { c->field = (int)strtol(val, NULL, 10); return 1; }

    I("assist_enabled", assist_enabled);
    I("passthrough", passthrough);
    I("grab", grab);
    I("loop_hz", loop_hz);
    I("hud", hud);
    I("use_filter", use_filter);
    I("graph_selection", graph_selection);
    F("filter_setting", filter_setting);
    F("steering_rate", steering_rate);
    F("rate_increase_with_speed", rate_increase_with_speed);
    F("target_slip_deg", target_slip_deg);
    F("target_slip_scale", target_slip_scale);
    F("self_steer_response", self_steer_response);
    F("damping_strength", damping_strength);
    F("max_self_steer_angle", max_self_steer_angle);
    F("countersteer_response", countersteer_response);
    F("max_dynamic_limit_reduction", max_dynamic_limit_reduction);
    F("steering_lock_deg", steering_lock_deg);
    F("wheelbase_m", wheelbase_m);
    F("stick_gamma", stick_gamma);
    F("deadzone", deadzone);
    F("steer_sign", steer_sign);
    F("yaw_sign", yaw_sign);
    F("lat_sign", lat_sign);
    F("fwd_sign", fwd_sign);
    I("swap_xz", swap_xz);
    I("invert_throttle", invert_throttle);
    I("ffb_enabled", ffb_enabled);
    I("ffb_road_cont", ffb_road_cont);
    F("ffb_lock_gain", ffb_lock_gain);
    F("ffb_spin_gain", ffb_spin_gain);
    F("ffb_road_gain", ffb_road_gain);
    I("invert_brake", invert_brake);

    I("udp_port", udp_port);
    if (strcmp(key, "gamepad_name") == 0) { set_str(c->gamepad_name, sizeof(c->gamepad_name), val); return 1; }
    if (strcmp(key, "shm_path") == 0) { set_str(c->shm_path, sizeof(c->shm_path), val); return 1; }

    #undef F
    #undef I
    fprintf(stderr, "simcontrol: unknown config key '%s'\n", key);
    return 0;
}

int sc_config_load(ScConfig *c, const char *path) {
    sc_config_defaults(c);
    if (!path || !*path) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    snprintf(c->path, sizeof(c->path), "%s", path);
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) parse_line(c, buf);
    fclose(f);

    if (c->loop_hz < 30) c->loop_hz = 30;
    if (c->loop_hz > 500) c->loop_hz = 500;
    if (c->steering_lock_deg < 5.f) c->steering_lock_deg = 5.f;
    if (c->wheelbase_m < 1.0f) c->wheelbase_m = 1.0f;
    if (c->deadzone < 0.f) c->deadzone = 0.f;
    if (c->deadzone > 0.5f) c->deadzone = 0.5f;
    if (c->stick_gamma < 0.5f) c->stick_gamma = 0.5f;
    if (c->steer_sign == 0.f) c->steer_sign = 1.f;
    if (c->yaw_sign == 0.f) c->yaw_sign = 1.f;
    if (c->lat_sign == 0.f) c->lat_sign = 1.f;
    if (c->fwd_sign == 0.f) c->fwd_sign = -1.f;
    if (c->graph_selection < 1) c->graph_selection = 1;
    if (c->graph_selection > 3) c->graph_selection = 3;
    c->filter_setting = clampf(c->filter_setting, 0.f, 1.f);

    sc_config_apply_filter(c);

    struct stat st;
    if (stat(path, &st) == 0) g_mtime = st.st_mtime;
    return 0;
}

int sc_config_reload_if_changed(ScConfig *c) {
    if (!c->path[0]) return 0;
    struct stat st;
    if (stat(c->path, &st) != 0) return 0;
    if (st.st_mtime == g_mtime) return 0;
    ScConfig tmp;
    if (sc_config_load(&tmp, c->path) != 0) return 0;
    *c = tmp;
    fprintf(stderr, "\nsimcontrol: reloaded %s\n", c->path);
    return 1;
}

void sc_config_apply_filter(ScConfig *c) {
    if (!c || !c->use_filter) return;
    float f = clampf(c->filter_setting, 0.f, 1.f);
    c->filter_setting = f;
    /* Simplified preset: derives all tuning knobs from one 0..1 value. */
    c->rate_increase_with_speed = (1.f - f) * 0.2f - 0.1f;
    c->self_steer_response = f * 0.5f + 0.12f;
    c->damping_strength = c->self_steer_response;
    c->max_self_steer_angle = 90.f;
    c->max_dynamic_limit_reduction = 3.f * f + 3.5f;
    c->countersteer_response = (1.f - f) * 0.2f + 0.1f;
    c->target_slip_scale = 0.95f - ((f - 0.5f) * 0.04f);
}

void sc_config_touch_mtime(const ScConfig *c) {
    if (!c || !c->path[0]) return;
    struct stat st;
    if (stat(c->path, &st) == 0) g_mtime = st.st_mtime;
}

int sc_config_save(const ScConfig *c) {
    if (!c || !c->path[0]) return -1;
    FILE *f = fopen(c->path, "w");
    if (!f) return -1;
    fprintf(f,
        "# SimControl — steering assist (AMS2 / AC Evo / AC Rally / Project CARS 2)\n"
        "# Edit this file while simcontrol is running; it reloads automatically.\n"
        "# The GTK config app also writes this file.\n"
        "# Keys in the terminal: a = assist on/off, p = passthrough, r = reload, q = quit\n"
        "\n"
        "assist_enabled = %d\n"
        "passthrough    = %d\n"
        "grab           = %d\n"
        "loop_hz        = %d\n"
        "hud            = %d\n"
        "use_filter     = %d\n"
        "filter_setting = %.4f\n"
        "graph_selection = %d\n"
        "\n"
        "# Leave empty to auto-pick the first gamepad. Substring, case-insensitive.\n"
        "gamepad_name = %s\n"
        "\n"
        "# Empty = try /dev/shm/$pcars2$ then /$pcars2\n"
        "shm_path = %s\n"
        "\n"
        "# AMS2 UDP (unused on this Proton build; SHM is the live path).\n"
        "udp_port = %d\n"
        "\n"
        "# --- steering ---\n"
        "steering_rate                = %.4f\n"
        "rate_increase_with_speed     = %.4f\n"
        "# Peak slip target in degrees (absolute).\n"
        "target_slip_deg              = %.4f\n"
        "target_slip_scale            = %.4f\n"
        "self_steer_response          = %.4f\n"
        "damping_strength             = %.4f\n"
        "max_self_steer_angle         = %.4f\n"
        "countersteer_response        = %.4f\n"
        "# 0–10, dynamic steering limit units.\n"
        "max_dynamic_limit_reduction  = %.4f\n"
        "\n"
        "# Assumed road-wheel lock (only used to normalise degree terms).\n"
        "steering_lock_deg            = %.4f\n"
        "wheelbase_m                  = %.4f\n"
        "\n"
        "# Stick shaping (the game will not apply pad gamma to a virtual wheel).\n"
        "stick_gamma                  = %.4f\n"
        "deadzone                     = %.4f\n"
        "\n"
        "# Flip if self-steer pushes INTO the slide, or left/right is reversed.\n"
        "steer_sign = %.0f\n"
        "yaw_sign   = %.0f\n"
        "lat_sign   = %.0f\n"
        "# -1 if HUD lim stays ~1.00 at speed (body Z points backward).\n"
        "fwd_sign   = %.0f\n"
        "swap_xz    = %d\n"
        "\n"
        "invert_throttle = %d\n"
        "invert_brake    = %d\n",
        c->assist_enabled, c->passthrough, c->grab, c->loop_hz, c->hud,
        c->use_filter, c->filter_setting, c->graph_selection,
        c->gamepad_name, c->shm_path, c->udp_port,
        c->steering_rate, c->rate_increase_with_speed,
        c->target_slip_deg, c->target_slip_scale,
        c->self_steer_response, c->damping_strength, c->max_self_steer_angle,
        c->countersteer_response, c->max_dynamic_limit_reduction,
        c->steering_lock_deg, c->wheelbase_m,
        c->stick_gamma, c->deadzone,
        c->steer_sign, c->yaw_sign, c->lat_sign, c->fwd_sign, c->swap_xz,
        c->invert_throttle, c->invert_brake);
    fclose(f);
    sc_config_touch_mtime(c);
    return 0;
}
