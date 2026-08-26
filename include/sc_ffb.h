#ifndef SC_FFB_H
#define SC_FFB_H

#include "sc_config.h"
#include "sc_input.h"
#include "sc_telem.h"

/* Telemetry-driven rumble feedback (Option A).
 *   big motor   <- wheel lock  (modulated by brake pressure)
 *   small motor <- wheelspin   (modulated by throttle)
 *                 + road/kerb texture (continuous floor optional)
 * Games without per-wheel data (ACE family) fall back to longitudinal
 * acceleration proxies. */

void sc_ffb_update(ScInput *in, const ScTelem *tm, const ScPadState *pad,
                   const ScConfig *cfg, double dt);
void sc_ffb_levels(float *strong01, float *weak01);

#endif
