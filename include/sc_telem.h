#ifndef SC_TELEM_H
#define SC_TELEM_H

#include "sc_config.h"
#include "sc_steer.h"

#define SC_SRC_NONE     0
#define SC_SRC_AMS2     1
#define SC_SRC_ACEVO    2
#define SC_SRC_ACRALLY  3
#define SC_SRC_PCARS2   4
#define SC_SRC_R3E      5
#define SC_SRC_RF1      6
#define SC_SRC_RF2      7

typedef struct {
    int   connected;
    int   playing;           /* GAME_INGAME_PLAYING */
    int   version;
    int   src;               /* SC_SRC_* */
    unsigned seq;
    float speed;
    float rpm;
    float local_vx, local_vy, local_vz;
    float ang_x, ang_y, ang_z;
    float game_steer;        /* mSteering -1..1 */
    int   front_grounded;
    int   rear_grounded;
    float wheel_rps[4];      /* per-wheel rev/s when the game exposes it */
    int   wheel_valid;       /* 1 = wheel_rps filled (AMS2/PCARS2) */
    char  car[64];
    char  track[64];
} ScTelem;

typedef struct ScTelemSrc ScTelemSrc;

ScTelemSrc *sc_telem_open(const ScConfig *cfg);
void         sc_telem_close(ScTelemSrc *t);
int          sc_telem_read(ScTelemSrc *t, const ScConfig *cfg, ScTelem *out);
void         sc_telem_to_vehicle(const ScTelem *t, const ScConfig *cfg, ScVehicle *v);

/* Extra sources (sc_telem_games.c) — additive, self-contained state. */
int  sc_r3e_read(ScTelem *out);      /* RaceRoom, native $R3E shm */
int  sc_rf1_read(ScTelem *out);      /* AMS1, $rFactorShared$ plugin shm */
int  sc_rf2_read(ScTelem *out);      /* rFactor 2 plugin telemetry map */

#endif
