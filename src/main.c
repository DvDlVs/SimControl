#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sc_config.h"
#include "sc_input.h"
#include "sc_ipc.h"
#include "sc_steer.h"
#include "sc_telem.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -c FILE   config file (default: ./simcontrol.conf or ~/.config/simcontrol/simcontrol.conf)\n"
        "  -l, --list          list input devices and exit\n"
        "  -p, --passthrough   ignore assist, pad → wheel 1:1\n"
        "  --no-grab           do not exclusive-grab the physical pad\n"
        "  -h, --help\n"
        "\n"
        "Keys (terminal):  a assist   p passthrough   r reload   q quit\n",
        argv0);
}

static int find_config(char *out, size_t n, const char *cli) {
    if (cli && *cli) { snprintf(out, n, "%s", cli); return 0; }
    if (access("./simcontrol.conf", R_OK) == 0) { snprintf(out, n, "./simcontrol.conf"); return 0; }
    const char *home = getenv("HOME");
    if (home) {
        snprintf(out, n, "%s/.config/simcontrol/simcontrol.conf", home);
        if (access(out, R_OK) == 0) return 0;
    }
    snprintf(out, n, "./simcontrol.conf");
    return -1;
}

static void setup_stdin(struct termios *saved, int *raw) {
    *raw = 0;
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, saved) != 0) return;
    struct termios t = *saved;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0) {
        fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
        *raw = 1;
    }
}

static void restore_stdin(struct termios *saved, int raw) {
    if (raw) tcsetattr(STDIN_FILENO, TCSANOW, saved);
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void hud(const ScConfig *cfg, const ScTelem *tm, const ScPadState *pad,
                const ScSteerOut *st, int assist_on) {
    if (!cfg->hud) return;
    const char *mode = assist_on ? "ON  " :
                       (cfg->passthrough ? "PASS" :
                        (!tm->connected ? "NOUDP" : "WAIT"));
    fprintf(stderr,
        "\rSimControl %s | %-16.16s | %5.0f km/h | in%+5.2f out%+5.2f t%.2f b%.2f | "
        "f%+5.1f r%+5.1f trv%+.0f yaw%+5.2f fade%.2f lim%.2f ss%+.2f   ",
        mode,
        tm->car[0] ? tm->car : "(no car)",
        tm->speed * 3.6f,
        pad->steer, st->output, pad->throttle, pad->brake,
        st->f_ang_deg, st->r_ang_deg, st->travel_deg,
        tm->ang_y, st->fade, st->limit, st->self_steer);
    fflush(stderr);
}

int main(int argc, char **argv) {
    const char *cli_cfg = NULL;
    int force_pass = 0;
    int no_grab = 0;

    static struct option longopts[] = {
        {"list", no_argument, 0, 'l'},
        {"passthrough", no_argument, 0, 'p'},
        {"no-grab", no_argument, 0, 1},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "c:lph", longopts, NULL)) != -1) {
        switch (opt) {
            case 'c': cli_cfg = optarg; break;
            case 'l': sc_input_list_devices(); return 0;
            case 'p': force_pass = 1; break;
            case 1:   no_grab = 1; break;
            case 'h': default: usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }

    char cfgpath[512];
    find_config(cfgpath, sizeof(cfgpath), cli_cfg);
    ScConfig cfg;
    if (sc_config_load(&cfg, cfgpath) != 0) {
        sc_config_defaults(&cfg);
        snprintf(cfg.path, sizeof(cfg.path), "%s", cfgpath);
        fprintf(stderr, "simcontrol: no config at %s — using defaults\n", cfgpath);
    } else {
        fprintf(stderr, "simcontrol: config %s\n", cfgpath);
    }
    if (force_pass) {
        cfg.passthrough = 1;
        fprintf(stderr, "simcontrol: PASSTHROUGH (no assist). Press p or restart without --passthrough.\n");
    }
    if (no_grab) cfg.grab = 0;

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    ScInput *in = sc_input_open(&cfg);
    if (!in) return 1;

    ScTelemSrc *tm = sc_telem_open(&cfg);
    ScSteerState steer;
    sc_steer_reset(&steer);
    ScIpc *ipc = sc_ipc_open();
    if (ipc) sc_ipc_write_snapshot(ipc, &cfg);

    double t_ipc_apply = 0;

    struct termios saved;
    int raw = 0;
    setup_stdin(&saved, &raw);
    if (raw) {
        fprintf(stderr, "keys: a=assist  p=passthrough  r=reload  q=quit\n");
    }

    double t_prev = now_sec();
    double t_hud = 0;
    double t_cfg = 0;

    double dt_nom = 1.0 / (double)cfg.loop_hz;
    double t_cfg_dt = 0;

    while (g_run) {
        double t0 = now_sec();
        float dt = (float)(t0 - t_prev);
        if (dt < 1e-4f) dt = (float)dt_nom;
        if (dt > 0.05f) dt = 0.05f;
        t_prev = t0;
        /* follow loop_hz hot-reloads without restart */
        if (cfg.loop_hz != 0 && t0 - t_cfg_dt > 1.0) {
            t_cfg_dt = t0;
            dt_nom = 1.0 / (double)cfg.loop_hz;
        }

        if (raw) {
            char ch;
            while (read(STDIN_FILENO, &ch, 1) == 1) {
                if (ch == 'q' || ch == 'Q' || ch == 3) g_run = 0;
                else if (ch == 'a' || ch == 'A') {
                    cfg.assist_enabled = !cfg.assist_enabled;
                    fprintf(stderr, "\nsimcontrol: assist %s\n", cfg.assist_enabled ? "ON" : "OFF");
                    if (ipc) sc_ipc_write_snapshot(ipc, &cfg);
                } else if (ch == 'p' || ch == 'P') {
                    cfg.passthrough = !cfg.passthrough;
                    fprintf(stderr, "\nsimcontrol: passthrough %s\n", cfg.passthrough ? "ON" : "OFF");
                    if (ipc) sc_ipc_write_snapshot(ipc, &cfg);
                } else if (ch == 'r' || ch == 'R') {
                    sc_config_load(&cfg, cfg.path);
                    fprintf(stderr, "\nsimcontrol: reloaded %s\n", cfg.path);
                    if (ipc) sc_ipc_write_snapshot(ipc, &cfg);
                }
            }
        }

        if (ipc && sc_ipc_apply_from_ui(ipc, &cfg)) {
            t_ipc_apply = t0;
            t_cfg = t0;
        } else if (t0 - t_cfg > 0.5 && (t0 - t_ipc_apply) > 1.0) {
            if (sc_config_reload_if_changed(&cfg) && ipc)
                sc_ipc_write_snapshot(ipc, &cfg);
            t_cfg = t0;
        }

        ScPadState pad;
        memset(&pad, 0, sizeof(pad));
        sc_input_poll(in, &cfg, &pad);

        ScTelem telem;
        memset(&telem, 0, sizeof(telem));
        sc_telem_read(tm, &cfg, &telem);

        ScVehicle veh;
        sc_telem_to_vehicle(&telem, &cfg, &veh);
        veh.brake = pad.brake;

        /* Don't fight the menus: force passthrough when not in session. */
        int live = telem.connected && telem.playing;
        int saved_pass = cfg.passthrough;
        if (!live) cfg.passthrough = 1;

        ScSteerOut so;
        sc_steer_step(&steer, &cfg, &veh, pad.steer, dt, &so);
        cfg.passthrough = saved_pass;

        float out_steer = so.output;
        int assist_on = cfg.assist_enabled && !cfg.passthrough && live && veh.valid;
        if (!assist_on) out_steer = pad.steer; /* already gamma'd */

        sc_input_emit(in, &pad, out_steer);

        if (ipc) sc_ipc_write_live(ipc, &cfg, &telem, &pad, &so, assist_on);

        if (t0 - t_hud > 0.15) {
            hud(&cfg, &telem, &pad, &so, assist_on);
            t_hud = t0;
        }

        double elapsed = now_sec() - t0;
        double sleep_s = dt_nom - elapsed;
        if (sleep_s > 0) {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = (long)(sleep_s * 1e9);
            nanosleep(&ts, NULL);
        }
    }

    fprintf(stderr, "\nsimcontrol: stopping\n");
    restore_stdin(&saved, raw);
    sc_ipc_close(ipc);
    sc_telem_close(tm);
    sc_input_close(in);
    return 0;
}
