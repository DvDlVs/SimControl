#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sc_ipc.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

_Static_assert(sizeof(ScIpc) <= SC_IPC_BYTES, "ScIpc exceeds mapped size");

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void ipc_init(ScIpc *ipc) {
    memset(ipc, 0, sizeof(*ipc));
    ipc->magic = SC_IPC_MAGIC;
    ipc->version = SC_IPC_VERSION;
    ipc->struct_size = (uint32_t)sizeof(ScIpc);
}

ScIpc *sc_ipc_open(void) {
    int fd = shm_open(SC_IPC_NAME, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        perror("simcontrol: shm_open " SC_IPC_NAME);
        return NULL;
    }
    if (ftruncate(fd, SC_IPC_BYTES) != 0) {
        perror("simcontrol: ftruncate " SC_IPC_NAME);
        close(fd);
        return NULL;
    }
    void *p = mmap(NULL, SC_IPC_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        perror("simcontrol: mmap " SC_IPC_NAME);
        return NULL;
    }
    ScIpc *ipc = (ScIpc *)p;
    if (ipc->magic != SC_IPC_MAGIC || ipc->version != SC_IPC_VERSION ||
        ipc->struct_size != (uint32_t)sizeof(ScIpc)) {
        ipc_init(ipc);
    }
    ipc->sc_running = 1;
    ipc->magic = SC_IPC_MAGIC;
    ipc->version = SC_IPC_VERSION;
    ipc->struct_size = (uint32_t)sizeof(ScIpc);
    sc_ipc_heartbeat(ipc);
    fprintf(stderr, "simcontrol: config app ipc %s (%zu bytes)\n", SC_IPC_NAME, sizeof(ScIpc));
    return ipc;
}

void sc_ipc_close(ScIpc *ipc) {
    if (!ipc) return;
    ipc->sc_running = 0;
    ipc->heartbeat_ns = 0;
    munmap(ipc, SC_IPC_BYTES);
}

void sc_ipc_heartbeat(ScIpc *ipc) {
    if (!ipc) return;
    ipc->sc_running = 1;
    ipc->heartbeat_ns = monotonic_ns();
}

void sc_ipc_write_snapshot(ScIpc *ipc, const ScConfig *cfg) {
    if (!ipc || !cfg) return;
    ipc->assist_enabled = cfg->assist_enabled;
    ipc->passthrough = cfg->passthrough;
    ipc->use_filter = cfg->use_filter;
    ipc->graph_selection = cfg->graph_selection;
    ipc->filter_setting = cfg->filter_setting;
    ipc->steering_rate = cfg->steering_rate;
    ipc->target_slip = cfg->target_slip_scale;
    ipc->rate_increase_with_speed = cfg->rate_increase_with_speed;
    ipc->self_steer_response = cfg->self_steer_response;
    ipc->damping_strength = cfg->damping_strength;
    ipc->max_self_steer_angle = cfg->max_self_steer_angle;
    ipc->countersteer_response = cfg->countersteer_response;
    ipc->max_dynamic_limit_reduction = cfg->max_dynamic_limit_reduction;
    ipc->stick_gamma = cfg->stick_gamma;
    ipc->deadzone = cfg->deadzone;
    ipc->target_slip_deg = cfg->target_slip_deg;
    ipc->steering_lock_deg = cfg->steering_lock_deg;
    ipc->wheelbase_m = cfg->wheelbase_m;
    ipc->steer_sign = cfg->steer_sign;
    ipc->yaw_sign = cfg->yaw_sign;
    ipc->lat_sign = cfg->lat_sign;
    ipc->fwd_sign = cfg->fwd_sign;
    ipc->swap_xz = cfg->swap_xz;
    ipc->invert_throttle = cfg->invert_throttle;
    ipc->invert_brake = cfg->invert_brake;
}

int sc_ipc_apply_from_ui(ScIpc *ipc, ScConfig *cfg) {
    if (!ipc || !cfg) return 0;
    uint32_t seq = ipc->settings_seq;
    if (seq == 0 || seq == ipc->settings_ack) return 0;

    cfg->assist_enabled = ipc->ui_assist_enabled;
    cfg->passthrough = ipc->ui_passthrough;
    cfg->use_filter = ipc->ui_use_filter;
    cfg->graph_selection = ipc->ui_graph_selection;
    cfg->filter_setting = ipc->ui_filter_setting;
    cfg->steering_rate = ipc->ui_steering_rate;
    cfg->target_slip_scale = ipc->ui_target_slip;
    cfg->rate_increase_with_speed = ipc->ui_rate_increase_with_speed;
    cfg->self_steer_response = ipc->ui_self_steer_response;
    cfg->damping_strength = ipc->ui_damping_strength;
    cfg->max_self_steer_angle = ipc->ui_max_self_steer_angle;
    cfg->countersteer_response = ipc->ui_countersteer_response;
    cfg->max_dynamic_limit_reduction = ipc->ui_max_dynamic_limit_reduction;
    cfg->stick_gamma = ipc->ui_stick_gamma;
    cfg->deadzone = ipc->ui_deadzone;
    if (ipc->ui_target_slip_deg > 0.5f)
        cfg->target_slip_deg = ipc->ui_target_slip_deg;
    if (ipc->ui_steering_lock_deg > 1.f)
        cfg->steering_lock_deg = ipc->ui_steering_lock_deg;
    if (ipc->ui_wheelbase_m > 0.5f)
        cfg->wheelbase_m = ipc->ui_wheelbase_m;
    cfg->steer_sign = ipc->ui_steer_sign;
    cfg->yaw_sign = ipc->ui_yaw_sign;
    cfg->lat_sign = ipc->ui_lat_sign;
    cfg->fwd_sign = ipc->ui_fwd_sign;
    cfg->swap_xz = ipc->ui_swap_xz;
    cfg->invert_throttle = ipc->ui_invert_throttle;
    cfg->invert_brake = ipc->ui_invert_brake;

    if (cfg->steer_sign == 0.f) cfg->steer_sign = 1.f;
    if (cfg->yaw_sign == 0.f) cfg->yaw_sign = 1.f;
    if (cfg->lat_sign == 0.f) cfg->lat_sign = 1.f;
    if (cfg->fwd_sign == 0.f) cfg->fwd_sign = -1.f;

    sc_config_apply_filter(cfg);
    sc_ipc_write_snapshot(ipc, cfg);
    ipc->settings_ack = seq;

    if (ipc->save_request) {
        if (sc_config_save(cfg) == 0)
            ipc->save_request = 0;
    }
    return 1;
}

void sc_ipc_write_live(ScIpc *ipc, const ScConfig *cfg, const ScTelem *tm,
                        const ScPadState *pad, const ScSteerOut *st, int assist_on) {
    if (!ipc) return;
    sc_ipc_heartbeat(ipc);
    ipc->connected = tm ? tm->connected : 0;
    ipc->game_id = (uint32_t)(tm ? tm->src : 0);
    ipc->playing = tm ? tm->playing : 0;
    ipc->assist_on = assist_on;
    ipc->speed_ms = tm ? tm->speed : 0.f;
    ipc->raw_steer = pad ? pad->steer : 0.f;
    ipc->final_steer = st ? st->output : 0.f;
    ipc->r_axle_hvel_angle = st ? st->r_ang_deg : 0.f;
    ipc->self_steer_strength = st ? st->self_steer_strength : 0.f;
    ipc->front_nd_slip = st ? st->front_nd_slip : 0.f;
    ipc->rear_nd_slip = st ? st->rear_nd_slip : 0.f;
    ipc->max_limit_reduction = st ? st->max_limit_reduction : 0.f;
    ipc->limit_reduction = st ? st->limit_reduction : 0.f;
    ipc->fade = st ? st->fade : 0.f;
    ipc->limit = st ? st->limit : 1.f;
    ipc->self_steer = st ? st->self_steer : 0.f;

    if (tm && tm->car[0])
        snprintf(ipc->car, sizeof(ipc->car), "%s", tm->car);
    else
        ipc->car[0] = 0;
    if (tm && tm->track[0])
        snprintf(ipc->track, sizeof(ipc->track), "%s", tm->track);
    else
        ipc->track[0] = 0;

    const char *mode = assist_on ? "ON" :
                       (cfg && cfg->passthrough ? "PASS" :
                        (tm && tm->connected ? "WAIT" : "NODATA"));
    snprintf(ipc->status, sizeof(ipc->status), "%s", mode);
}
