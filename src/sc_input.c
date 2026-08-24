#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sc_input.h"
#include "sc_math.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <ctype.h>
#include <math.h>

#define STEER_ABS_MAX 32767
#define PEDAL_ABS_MAX 32767

typedef struct {
    int code;
    int min, max;
    int value;
    int present;
} Axis;

struct ScInput {
    int pad_fd;
    int ui_fd;
    char pad_name[256];
    Axis abs_x, abs_y, abs_z, abs_rz, abs_gas, abs_brake, abs_hatx, abs_haty, abs_rx, abs_ry;
    int has_gas, has_brake_axis;
    int auto_inv_throt, auto_inv_brake;
    int key_codes[KEY_MAX];
    int nkey_codes;
    ScPadState last;
    int btn_state[KEY_MAX];
    int out_steer, out_gas, out_brk, out_hatx, out_haty;
    int out_btn[KEY_MAX];
    int have_out;
};

static int read_name(int fd, char *buf, size_t n) {
    memset(buf, 0, n);
    if (ioctl(fd, EVIOCGNAME(n - 1), buf) < 0) {
        snprintf(buf, n, "unknown");
        return -1;
    }
    return 0;
}

static int test_bit(int bit, const unsigned char *arr) {
    return (arr[bit / 8] >> (bit % 8)) & 1;
}

static void emit(int fd, int type, int code, int val);
static int map_key(int code);

static int looks_like_pad(int fd, char *name, size_t nlen) {
    unsigned char ev[(EV_MAX + 7) / 8];
    unsigned char abs[(ABS_MAX + 7) / 8];
    unsigned char key[(KEY_MAX + 7) / 8];
    memset(ev, 0, sizeof(ev));
    memset(abs, 0, sizeof(abs));
    memset(key, 0, sizeof(key));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev) < 0) return 0;
    if (!test_bit(EV_ABS, ev) || !test_bit(EV_KEY, ev)) return 0;
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) < 0) return 0;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key) < 0) return 0;
    if (!test_bit(ABS_X, abs)) return 0;
    int has_btn = test_bit(BTN_GAMEPAD, key) || test_bit(BTN_SOUTH, key) ||
                  test_bit(BTN_A, key) || test_bit(BTN_JOYSTICK, key);
    if (!has_btn) return 0;
    read_name(fd, name, nlen);
    /* Skip our own virtual wheel and obvious non-pads. */
    if (strcasestr(name, "SimControl ")) return 0;
    if (strcasestr(name, "Driving Force Racing Wheel")) return 0;
    if (strcasestr(name, "Mouse") || strcasestr(name, "Keyboard")) return 0;
    return 1;
}

static void fill_axis(int fd, Axis *a, int code) {
    struct input_absinfo info;
    memset(&info, 0, sizeof(info));
    a->code = code;
    a->present = 0;
    if (ioctl(fd, EVIOCGABS(code), &info) < 0) return;
    a->min = info.minimum;
    a->max = info.maximum;
    a->value = info.value;
    a->present = (a->max != a->min);
}

static float axis_norm_m11(const Axis *a) {
    if (!a->present) return 0.f;
    float span = (float)(a->max - a->min);
    if (fabsf(span) < 1.f) return 0.f;
    float n = ((float)a->value - (float)a->min) / span; /* 0..1 */
    return n * 2.f - 1.f;
}

static float axis_norm_01(const Axis *a, int invert) {
    if (!a->present) return 0.f;
    float span = (float)(a->max - a->min);
    if (fabsf(span) < 1.f) return 0.f;
    float n = ((float)a->value - (float)a->min) / span;
    n = sc_clamp01(n);
    return invert ? (1.f - n) : n;
}

static int rest_is_pressed(const Axis *a) {
    if (!a->present) return 0;
    float span = (float)(a->max - a->min);
    if (span < 1.f) return 0;
    float n = ((float)a->value - (float)a->min) / span;
    return n > 0.8f;
}

static void refresh_axes(ScInput *in) {
    fill_axis(in->pad_fd, &in->abs_x, ABS_X);
    fill_axis(in->pad_fd, &in->abs_y, ABS_Y);
    fill_axis(in->pad_fd, &in->abs_z, ABS_Z);
    fill_axis(in->pad_fd, &in->abs_rz, ABS_RZ);
    fill_axis(in->pad_fd, &in->abs_gas, ABS_GAS);
    fill_axis(in->pad_fd, &in->abs_brake, ABS_BRAKE);
    fill_axis(in->pad_fd, &in->abs_hatx, ABS_HAT0X);
    fill_axis(in->pad_fd, &in->abs_haty, ABS_HAT0Y);
    fill_axis(in->pad_fd, &in->abs_rx, ABS_RX);
    fill_axis(in->pad_fd, &in->abs_ry, ABS_RY);
}

static void refresh_buttons(ScInput *in) {
    unsigned char key[(KEY_MAX + 7) / 8];
    memset(key, 0, sizeof(key));
    if (ioctl(in->pad_fd, EVIOCGKEY(sizeof(key)), key) < 0) return;
    for (int i = 0; i < in->nkey_codes; i++) {
        int c = in->key_codes[i];
        if (c >= 0 && c < KEY_MAX)
            in->btn_state[c] = test_bit(c, key);
    }
}

static float apply_deadzone(float x, float dz) {
    float ax = fabsf(x);
    if (ax <= dz) return 0.f;
    float s = sc_signf(x);
    return s * (ax - dz) / (1.f - dz);
}

static int open_pad(const ScConfig *cfg, char *name_out, size_t nlen) {
    DIR *d = opendir("/dev/input");
    if (!d) {
        fprintf(stderr, "simcontrol: cannot open /dev/input: %s\n", strerror(errno));
        return -1;
    }
    int best = -1;
    char best_name[256] = {0};
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        char nm[256];
        if (!looks_like_pad(fd, nm, sizeof(nm))) { close(fd); continue; }
        int match = 1;
        if (cfg->gamepad_name[0]) {
            match = strcasestr(nm, cfg->gamepad_name) != NULL;
        }
        if (match) {
            if (best >= 0) close(best);
            best = fd;
            snprintf(best_name, sizeof(best_name), "%s", nm);
            if (cfg->gamepad_name[0]) break; /* first match of named pad */
            /* else keep scanning; last pad wins unless named */
        } else {
            close(fd);
        }
    }
    closedir(d);
    if (best < 0) {
        fprintf(stderr, "simcontrol: no gamepad found. Plug one in, or set gamepad_name= in simcontrol.conf\n");
        fprintf(stderr, "     (try: ./simcontrol --list)\n");
        return -1;
    }
    snprintf(name_out, nlen, "%s", best_name);
    return best;
}

static int setup_uinput(ScInput *in) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "simcontrol: cannot open /dev/uinput: %s\n", strerror(errno));
        fprintf(stderr, "     sudo cp udev/99-simcontrol.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules\n");
        fprintf(stderr, "     sudo usermod -aG input $USER   (then log out/in)\n");
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);

    /* Joystick buttons only — BTN_GAMEPAD makes Wine expose XInput, and AMS2
     * Wheel mode ignores XInput pads. G29-style DirectInput uses BTN_TRIGGER+. */
    for (int k = BTN_TRIGGER; k <= BTN_BASE6; k++) ioctl(fd, UI_SET_KEYBIT, k);

    /* One axis per function. Duplicates (WHEEL+X, GAS+Y, BRAKE+Z) make AMS2
     * say "multiple inputs detected" while assigning controls. */
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);
    ioctl(fd, UI_SET_ABSBIT, ABS_Z);
    ioctl(fd, UI_SET_ABSBIT, ABS_RX);
    ioctl(fd, UI_SET_ABSBIT, ABS_RY);
    ioctl(fd, UI_SET_ABSBIT, ABS_HAT0X);
    ioctl(fd, UI_SET_ABSBIT, ABS_HAT0Y);

    struct uinput_abs_setup abs;
    struct uinput_setup usetup;

    memset(&abs, 0, sizeof(abs));
    abs.absinfo.minimum = -STEER_ABS_MAX;
    abs.absinfo.maximum =  STEER_ABS_MAX;
    abs.absinfo.fuzz = 0;
    abs.absinfo.flat = 0;
    abs.code = ABS_X;
    ioctl(fd, UI_ABS_SETUP, &abs);

    memset(&abs, 0, sizeof(abs));
    abs.absinfo.minimum = 0;
    abs.absinfo.maximum = PEDAL_ABS_MAX;
    abs.absinfo.value = 0; /* released — some kernels report max until the first event */
    abs.code = ABS_Y; /* throttle */
    ioctl(fd, UI_ABS_SETUP, &abs);
    abs.code = ABS_Z; /* brake */
    ioctl(fd, UI_ABS_SETUP, &abs);

    memset(&abs, 0, sizeof(abs));
    abs.absinfo.minimum = -STEER_ABS_MAX;
    abs.absinfo.maximum =  STEER_ABS_MAX;
    abs.absinfo.value = 0;
    abs.code = ABS_RX; /* look X (right stick) */
    ioctl(fd, UI_ABS_SETUP, &abs);
    abs.code = ABS_RY; /* look Y */
    ioctl(fd, UI_ABS_SETUP, &abs);

    memset(&abs, 0, sizeof(abs));
    abs.absinfo.minimum = -1;
    abs.absinfo.maximum = 1;
    abs.absinfo.value = 0;
    abs.code = ABS_HAT0X;
    ioctl(fd, UI_ABS_SETUP, &abs);
    abs.code = ABS_HAT0Y;
    ioctl(fd, UI_ABS_SETUP, &abs);

    memset(&usetup, 0, sizeof(usetup));
    /* Generic DirectInput wheel. Do NOT use Logitech VID/PID: AMS2's G29
     * profile talks to the Logitech SDK and gets no HID reports from uinput. */
    snprintf(usetup.name, sizeof(usetup.name), "SimControl Racing Wheel");
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor  = 0x0001;
    usetup.id.product = 0x0001;
    usetup.id.version = 1;
    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) {
        fprintf(stderr, "simcontrol: UI_DEV_SETUP failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "simcontrol: UI_DEV_CREATE failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    in->ui_fd = fd;
    /* Give udev a moment to pick the device up. */
    usleep(80000);
    return 0;
}

static void emit(int fd, int type, int code, int val) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = val;
    if (write(fd, &ev, sizeof(ev)) < 0) { /* ignore */ }
}

ScInput *sc_input_open(const ScConfig *cfg) {
    ScInput *in = calloc(1, sizeof(*in));
    if (!in) return NULL;
    in->pad_fd = -1;
    in->ui_fd = -1;

    in->pad_fd = open_pad(cfg, in->pad_name, sizeof(in->pad_name));
    if (in->pad_fd < 0) { free(in); return NULL; }

    refresh_axes(in);
    /* Rest-position detection: xpad drivers occasionally report triggers
     * saturated right after open, and squeezing a trigger during start
     * fools a single sample too — either way the pedals end up inverted
     * until the next restart. Sample for ~300 ms and trust the MINIMUM
     * observed value (true rest can only go down from any transient). */
    {
        int g_min  = in->abs_gas.value,  rz_min = in->abs_rz.value;
        int b_min  = in->abs_brake.value, z_min = in->abs_z.value;
        for (int i = 0; i < 12; i++) {
            usleep(25000);
            refresh_axes(in);
            if (in->abs_gas.value   < g_min)  g_min  = in->abs_gas.value;
            if (in->abs_rz.value    < rz_min) rz_min = in->abs_rz.value;
            if (in->abs_brake.value < b_min)  b_min  = in->abs_brake.value;
            if (in->abs_z.value     < z_min)  z_min  = in->abs_z.value;
        }
        in->abs_gas.value   = g_min;
        in->abs_rz.value    = rz_min;
        in->abs_brake.value = b_min;
        in->abs_z.value     = z_min;
    }
    in->has_gas = in->abs_gas.present;
    in->has_brake_axis = in->abs_brake.present;
    in->auto_inv_throt = rest_is_pressed(in->has_gas ? &in->abs_gas : &in->abs_rz);
    in->auto_inv_brake = rest_is_pressed(in->has_brake_axis ? &in->abs_brake : &in->abs_z);

    unsigned char key[(KEY_MAX + 7) / 8];
    memset(key, 0, sizeof(key));
    ioctl(in->pad_fd, EVIOCGBIT(EV_KEY, sizeof(key)), key);
    in->nkey_codes = 0;
    for (int k = 0; k < KEY_MAX; k++) {
        if (test_bit(k, key) && in->nkey_codes < KEY_MAX) {
            /* skip mouse buttons */
            if (k >= BTN_MOUSE && k < BTN_JOYSTICK) continue;
            in->key_codes[in->nkey_codes++] = k;
        }
    }

    if (cfg->grab) {
        if (ioctl(in->pad_fd, EVIOCGRAB, 1) < 0) {
            fprintf(stderr, "simcontrol: EVIOCGRAB failed (%s) — game may still see the pad\n", strerror(errno));
        }
    }

    if (setup_uinput(in) != 0) {
        if (cfg->grab) ioctl(in->pad_fd, EVIOCGRAB, 0);
        close(in->pad_fd);
        free(in);
        return NULL;
    }
    fprintf(stderr, "simcontrol: pad '%s'\n", in->pad_name);
    fprintf(stderr, "simcontrol: virtual DirectInput wheel 'SimControl Racing Wheel'\n");
    fprintf(stderr, "simcontrol: steer X=%d [%d..%d]  throttle %s=%d [%d..%d]%s  brake %s=%d [%d..%d]%s\n",
            in->abs_x.value, in->abs_x.min, in->abs_x.max,
            in->has_gas ? "GAS" : "RZ",
            in->has_gas ? in->abs_gas.value : in->abs_rz.value,
            in->has_gas ? in->abs_gas.min : in->abs_rz.min,
            in->has_gas ? in->abs_gas.max : in->abs_rz.max,
            in->auto_inv_throt ? " (auto-invert)" : "",
            in->has_brake_axis ? "BRAKE" : "Z",
            in->has_brake_axis ? in->abs_brake.value : in->abs_z.value,
            in->has_brake_axis ? in->abs_brake.min : in->abs_z.min,
            in->has_brake_axis ? in->abs_brake.max : in->abs_z.max,
            in->auto_inv_brake ? " (auto-invert)" : "");

    fprintf(stderr, "simcontrol: buttons  A/B/X/Y  LB/RB  Back/Start  LS/RS  Guide  + D-pad + look stick\n");

    /* Push a known rest state before the game opens the device. */
    emit(in->ui_fd, EV_ABS, ABS_X, 0);
    emit(in->ui_fd, EV_ABS, ABS_Y, 0);
    emit(in->ui_fd, EV_ABS, ABS_Z, 0);
    emit(in->ui_fd, EV_ABS, ABS_RX, 0);
    emit(in->ui_fd, EV_ABS, ABS_RY, 0);
    emit(in->ui_fd, EV_ABS, ABS_HAT0X, 0);
    emit(in->ui_fd, EV_ABS, ABS_HAT0Y, 0);
    for (int k = BTN_TRIGGER; k <= BTN_BASE6; k++)
        emit(in->ui_fd, EV_KEY, k, 0);
    emit(in->ui_fd, EV_SYN, SYN_REPORT, 0);
    return in;
}

void sc_input_close(ScInput *in) {
    if (!in) return;
    if (in->ui_fd >= 0) {
        ioctl(in->ui_fd, UI_DEV_DESTROY);
        close(in->ui_fd);
    }
    if (in->pad_fd >= 0) {
        ioctl(in->pad_fd, EVIOCGRAB, 0);
        close(in->pad_fd);
    }
    free(in);
}

static void apply_event(ScInput *in, const struct input_event *ev) {
    if (ev->type == EV_ABS) {
        if (ev->code == ABS_X) in->abs_x.value = ev->value;
        else if (ev->code == ABS_Y) in->abs_y.value = ev->value;
        else if (ev->code == ABS_Z) in->abs_z.value = ev->value;
        else if (ev->code == ABS_RZ) in->abs_rz.value = ev->value;
        else if (ev->code == ABS_GAS) in->abs_gas.value = ev->value;
        else if (ev->code == ABS_BRAKE) in->abs_brake.value = ev->value;
        else if (ev->code == ABS_HAT0X) in->abs_hatx.value = ev->value;
        else if (ev->code == ABS_HAT0Y) in->abs_haty.value = ev->value;
        else if (ev->code == ABS_RX) in->abs_rx.value = ev->value;
        else if (ev->code == ABS_RY) in->abs_ry.value = ev->value;
    } else if (ev->type == EV_KEY) {
        if (ev->code < KEY_MAX) in->btn_state[ev->code] = ev->value ? 1 : 0;
    }
}

int sc_input_poll(ScInput *in, const ScConfig *cfg, ScPadState *pad) {
    struct input_event ev;
    while (read(in->pad_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        apply_event(in, &ev);
    }
    /* EVIOCGABS is the current hardware value — the 64-event evdev queue
     * overflows on a noisy Xbox pad and we would keep a stale 255 on Z/RZ
     * (full pedals / dead steer). */
    refresh_axes(in);
    refresh_buttons(in);

    float steer = axis_norm_m11(&in->abs_x) * cfg->steer_sign;
    steer = apply_deadzone(sc_clamp1(steer), cfg->deadzone);
    pad->steer_raw = steer;
    float g = cfg->stick_gamma < 0.5f ? 1.f : cfg->stick_gamma;
    pad->steer = sc_signf(steer) * powf(fabsf(steer), g);

    int inv_t = cfg->invert_throttle || in->auto_inv_throt;
    int inv_b = cfg->invert_brake || in->auto_inv_brake;
    if (in->has_gas) pad->throttle = axis_norm_01(&in->abs_gas, inv_t);
    else             pad->throttle = axis_norm_01(&in->abs_rz, inv_t);

    if (in->has_brake_axis) pad->brake = axis_norm_01(&in->abs_brake, inv_b);
    else                    pad->brake = axis_norm_01(&in->abs_z, inv_b);

    pad->clutch = 0.f;
    pad->hat_x = in->abs_hatx.present ? (in->abs_hatx.value > 0 ? 1 : (in->abs_hatx.value < 0 ? -1 : 0)) : 0;
    pad->hat_y = in->abs_haty.present ? (in->abs_haty.value > 0 ? 1 : (in->abs_haty.value < 0 ? -1 : 0)) : 0;

    pad->nkeys = 0;
    for (int i = 0; i < in->nkey_codes && pad->nkeys < SC_BTN_MAX; i++) {
        int c = in->key_codes[i];
        if (in->btn_state[c]) {
            pad->keys[pad->nkeys++] = c;
        }
    }
    in->last = *pad;
    return 0;
}

static int map_key(int code) {
    /* Xbox/SDL buttons → joystick buttons 1–12 (DirectInput / AMS2 Custom). */
    switch (code) {
        case BTN_SOUTH:   return BTN_TRIGGER; /* A  → 1 */
        case BTN_EAST:    return BTN_THUMB;   /* B  → 2 */
        case BTN_WEST:    return BTN_THUMB2;  /* X  → 3 */
        case BTN_NORTH:   return BTN_TOP;     /* Y  → 4 */
        case BTN_TL:      return BTN_TOP2;    /* LB → 5 */
        case BTN_TR:      return BTN_PINKIE;  /* RB → 6 */
        case BTN_SELECT:  return BTN_BASE;    /* Back → 7 */
        case BTN_START:   return BTN_BASE2;   /* Start → 8 */
        case BTN_THUMBL:  return BTN_BASE3;   /* LS → 9 */
        case BTN_THUMBR:  return BTN_BASE4;   /* RS → 10 */
        case BTN_MODE:    return BTN_BASE5;   /* Guide → 11 */
#ifdef BTN_C
        case BTN_C:       return BTN_BASE6;
#endif
        /* Already a joystick-range button: pass through. */
        default:
            if (code >= BTN_TRIGGER && code <= BTN_BASE6) return code;
            return 0; /* skip digital LT/RT (BTN_TL2/TR2) — they duplicate pedals */
    }
}

int sc_input_emit(ScInput *in, const ScPadState *pad, float steer_out) {
    int sx = (int)lroundf(sc_clamp1(steer_out) * (float)STEER_ABS_MAX);
    int gas = (int)lroundf(sc_clamp01(pad->throttle) * (float)PEDAL_ABS_MAX);
    int brk = (int)lroundf(sc_clamp01(pad->brake) * (float)PEDAL_ABS_MAX);

    /* Analog every tick: Wine/AMS2 miss a rest-position event that happened
     * before they opened the device, and then the wheel sticks at full
     * throttle/brake (uinput reporting max). */
    float lookx = apply_deadzone(axis_norm_m11(&in->abs_rx), 0.18f);
    float looky = apply_deadzone(axis_norm_m11(&in->abs_ry), 0.18f);
    int rx = (int)lroundf(sc_clamp1(lookx) * (float)STEER_ABS_MAX);
    int ry = (int)lroundf(sc_clamp1(looky) * (float)STEER_ABS_MAX);

    emit(in->ui_fd, EV_ABS, ABS_X, sx);
    emit(in->ui_fd, EV_ABS, ABS_Y, gas);
    emit(in->ui_fd, EV_ABS, ABS_Z, brk);
    emit(in->ui_fd, EV_ABS, ABS_RX, rx);
    emit(in->ui_fd, EV_ABS, ABS_RY, ry);
    emit(in->ui_fd, EV_ABS, ABS_HAT0X, pad->hat_x);
    emit(in->ui_fd, EV_ABS, ABS_HAT0Y, pad->hat_y);
    in->out_steer = sx;
    in->out_gas = gas;
    in->out_brk = brk;
    in->out_hatx = pad->hat_x;
    in->out_haty = pad->hat_y;

    for (int i = 0; i < in->nkey_codes; i++) {
        int src = in->key_codes[i];
        int dst = map_key(src);
        if (!dst) continue;
        int val = in->btn_state[src] ? 1 : 0;
        emit(in->ui_fd, EV_KEY, dst, val);
        in->out_btn[dst] = val;
    }
    emit(in->ui_fd, EV_SYN, SYN_REPORT, 0);
    in->have_out = 1;
    return 0;
}

void sc_input_list_devices(void) {
    DIR *d = opendir("/dev/input");
    if (!d) { perror("/dev/input"); return; }
    struct dirent *de;
    printf("Input devices:\n");
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        char nm[256];
        read_name(fd, nm, sizeof(nm));
        int pad = looks_like_pad(fd, nm, sizeof(nm));
        printf("  %s  %-40s %s\n", path, nm, pad ? "[gamepad]" : "");
        close(fd);
    }
    closedir(d);
}

const char *sc_input_pad_name(const ScInput *in) { return in ? in->pad_name : ""; }
int sc_input_fd(const ScInput *in) { return in ? in->pad_fd : -1; }
