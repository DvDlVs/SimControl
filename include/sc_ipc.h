#ifndef SC_IPC_H
#define SC_IPC_H

#include "sc_config.h"
#include "sc_input.h"
#include "sc_steer.h"
#include "sc_telem.h"

#include <stdint.h>

#define SC_IPC_MAGIC   0x314C4353u /* 'SCL1' */
#define SC_IPC_VERSION 2
#define SC_IPC_NAME    "/simcontrol_ipc"
#define SC_IPC_BYTES   4096

/*
 * Bidirectional POSIX shm between `simcontrol` and ui/simcontrol_config.py.
 * Keep field order in sync with the ctypes Structure in the Python app.
 *
 * daemon writes: heartbeat, live telemetry, settings snapshot, settings_ack
 * UI writes:  ui_* fields, then increments settings_seq (optionally save_request)
 */
typedef struct ScIpc {
    uint32_t magic;
    uint32_t version;
    uint32_t struct_size;
    uint32_t _pad0;
    uint64_t heartbeat_ns;

    int32_t  connected;
    int32_t  playing;
    int32_t  assist_on;
    int32_t  sc_running;

    float    speed_ms;
    float    raw_steer;
    float    final_steer;
    float    r_axle_hvel_angle;
    float    self_steer_strength;
    float    front_nd_slip;
    float    rear_nd_slip;
    float    max_limit_reduction;
    float    limit_reduction;
    float    fade;
    float    limit;
    float    self_steer;

    char     car[64];
    char     track[64];
    char     status[16];

    uint32_t settings_ack;
    uint32_t game_id;        /* SC_SRC_* of the attached game, 0 = none */
    int32_t  assist_enabled;
    int32_t  passthrough;
    int32_t  use_filter;
    int32_t  graph_selection;
    float    filter_setting;
    float    steering_rate;
    float    target_slip;
    float    rate_increase_with_speed;
    float    self_steer_response;
    float    damping_strength;
    float    max_self_steer_angle;
    float    countersteer_response;
    float    max_dynamic_limit_reduction;
    float    stick_gamma;
    float    deadzone;

    uint32_t settings_seq;
    uint32_t save_request;
    int32_t  ui_assist_enabled;
    int32_t  ui_passthrough;
    int32_t  ui_use_filter;
    int32_t  ui_graph_selection;
    float    ui_filter_setting;
    float    ui_steering_rate;
    float    ui_target_slip;
    float    ui_rate_increase_with_speed;
    float    ui_self_steer_response;
    float    ui_damping_strength;
    float    ui_max_self_steer_angle;
    float    ui_countersteer_response;
    float    ui_max_dynamic_limit_reduction;
    float    ui_stick_gamma;
    float    ui_deadzone;

    float    target_slip_deg;
    float    steering_lock_deg;
    float    wheelbase_m;
    float    steer_sign;
    float    yaw_sign;
    float    lat_sign;
    float    fwd_sign;
    int32_t  swap_xz;
    int32_t  invert_throttle;
    int32_t  invert_brake;

    float    ui_target_slip_deg;
    float    ui_steering_lock_deg;
    float    ui_wheelbase_m;
    float    ui_steer_sign;
    float    ui_yaw_sign;
    float    ui_lat_sign;
    float    ui_fwd_sign;
    int32_t  ui_swap_xz;
    int32_t  ui_invert_throttle;
    int32_t  ui_invert_brake;
} ScIpc;

ScIpc *sc_ipc_open(void);
void    sc_ipc_close(ScIpc *ipc);
void    sc_ipc_heartbeat(ScIpc *ipc);
void    sc_ipc_write_snapshot(ScIpc *ipc, const ScConfig *cfg);
void    sc_ipc_write_live(ScIpc *ipc, const ScConfig *cfg, const ScTelem *tm,
                           const ScPadState *pad, const ScSteerOut *st, int assist_on);
/* Returns 1 if UI settings were applied into cfg. */
int     sc_ipc_apply_from_ui(ScIpc *ipc, ScConfig *cfg);

#endif
