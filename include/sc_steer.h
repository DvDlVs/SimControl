#ifndef SC_STEER_H
#define SC_STEER_H

#include "sc_config.h"
#include "sc_math.h"

typedef struct {
    float local_vx;          /* right + */
    float local_vz;          /* forward + */
    float yaw_rate;          /* rad/s, left-handed Y-up (right turn + if AMS2 matches AC) */
    float speed;             /* m/s */
    float brake;             /* 0..1, from pad */
    float front_grounded;    /* 0..1 already smoothed by caller or raw 0/1 */
    int   valid;
} ScVehicle;

typedef struct {
    float initial;           /* smoothed stick -1..1 */
    float abs_initial;
    float output;            /* assisted -1..1 */
    float fade;
    float f_ang_deg;
    float r_ang_deg;
    float limit;
    float self_steer;
    float travel_deg;
    float self_steer_strength;
    float limit_reduction;
    float max_limit_reduction;
    float front_nd_slip;
    float rear_nd_slip;
} ScSteerOut;

typedef struct {
    ScSmooth steering;
    ScSmooth abs_steering;
    ScSmooth self_steer;
    ScSmooth limit;
    ScSmooth grounded;
    ScSmooth counter;
    ScSmooth front_slip;
    ScSmooth rear_slip;
    int inited;
} ScSteerState;

void sc_steer_reset(ScSteerState *st);
void sc_steer_step(ScSteerState *st, const ScConfig *cfg,
                    const ScVehicle *veh, float raw_steer, float dt,
                    ScSteerOut *out);

#endif
