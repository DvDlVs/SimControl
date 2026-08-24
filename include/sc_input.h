#ifndef SC_INPUT_H
#define SC_INPUT_H

#include "sc_config.h"

#define SC_BTN_MAX 32

typedef struct {
    float steer;      /* -1..1 after deadzone+gamma */
    float steer_raw;  /* -1..1 after deadzone, before gamma */
    float throttle;   /* 0..1 */
    float brake;      /* 0..1 */
    float clutch;     /* 0..1 */
    int   hat_x;      /* -1..1 */
    int   hat_y;
    int   keys[SC_BTN_MAX];
    int   nkeys;
} ScPadState;

typedef struct ScInput ScInput;

ScInput *sc_input_open(const ScConfig *cfg);
void      sc_input_close(ScInput *in);
int       sc_input_poll(ScInput *in, const ScConfig *cfg, ScPadState *pad);
int       sc_input_emit(ScInput *in, const ScPadState *pad, float steer_out);
void      sc_input_list_devices(void);
const char *sc_input_pad_name(const ScInput *in);
int       sc_input_fd(const ScInput *in);

#endif
