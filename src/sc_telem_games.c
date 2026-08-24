#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sc_telem.h"
#include "sc_math.h"
#include "r3e.h"

/*
 * Extra telemetry sources, self-contained (own fds + state below; the
 * daemon is single-threaded). Nothing here touches the AMS2/PCARS2/AC paths.
 *
 *   RaceRoom — native shared memory ($R3E), read through the game's
 *              wine-mapping fd like the AC sources. API v3.x.
 *   AMS1     — "$rFactorShared$" written by the community shm plugin
 *              (github.com/dallongo/rFactorSharedMemoryMap); layout per
 *              simapi's rfdata.h. Requires that DLL in the game prefix.
 *   rFactor2 — telemetry map written by TheIronWolf's
 *              rF2SharedMemoryMapPlugin; layout per simapi's rf2data.h
 *              (pack 4, double-buffered with version guards). Requires the
 *              plugin DLL in the game's Plugins folder.
 */

#define R3E_PREFIX_MAX 16384
#define RF1_PREFIX_BYTES 616   /* rfShared up to vehicles[64] */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static double games_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void copy_printable(char *dst, size_t n, const unsigned char *src, size_t maxn) {
    size_t i = 0;
    if (!dst || n == 0) return;
    while (i + 1 < n && i < maxn && src[i] >= 32 && src[i] < 127) {
        dst[i] = (char)src[i];
        i++;
    }
    dst[i] = 0;
}

/* Per-read sanity gate: maps without double buffering (and torn reads in
 * general) can mix old/new floats into finite-but-absurd values that would
 * slam the assist for a few ticks. Reject the sample instead. */
static int vel_sane(float vx, float vy, float vz, float yaw, float spd) {
    if (!isfinite(vx) || !isfinite(vy) || !isfinite(vz) || !isfinite(yaw))
        return 0;
    if (fabsf(vx) > 60.f || fabsf(vy) > 80.f || fabsf(vz) > 130.f)
        return 0;
    if (!isfinite(yaw) || fabsf(yaw) > 12.f)
        return 0;
    if (!isfinite(spd) || spd < -5.f || spd > 200.f)
        return 0;
    /* scalar speed must agree with the vector magnitude */
    {
        float mag = sqrtf(vx * vx + vz * vz);
        if (fabsf(spd - mag) > 20.f && fabsf(spd - mag) > 0.45f * spd)
            return 0;
    }
    return 1;
}

/* Delta gate: a sample whose jump vs the last accepted one exceeds what
 * physics allows in one tick is a tear even when magnitudes look sane.
 * Persistent jumps (respawn/teleport) force a re-sync after ~12 rejects.
 * State is per-source; reset on attach. */
typedef struct { float vx, vy, vz, yaw; int have, rej; } dgate;
static int delta_sane(dgate *g, float vx, float vy, float vz, float yaw) {
    const float DVX = 18.f, DVZ = 28.f, DVY = 40.f, DYAW = 2.5f;
    if (!g->have) {
        g->vx = vx; g->vy = vy; g->vz = vz; g->yaw = yaw;
        g->have = 1; g->rej = 0;
        return 1;
    }
    if (fabsf(vx - g->vx) < DVX && fabsf(vz - g->vz) < DVZ &&
        fabsf(vy - g->vy) < DVY && fabsf(yaw - g->yaw) < DYAW) {
        g->vx = vx; g->vy = vy; g->vz = vz; g->yaw = yaw;
        g->rej = 0;
        return 1;
    }
    if (++g->rej >= 12) {
        g->vx = vx; g->vy = vy; g->vz = vz; g->yaw = yaw;
        g->rej = 0;
        return 1;
    }
    return 0;
}

static int pid_matches_cmdline(int pid, const char *needle) {
    char path[64], buf[1024];
    ssize_t n;
    int fd, i;
    if (pid <= 0 || !needle || !*needle) return 0;
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    for (i = 0; i < (int)n; i++) {
        if (buf[i] == 0) buf[i] = ' ';
    }
    return strcasestr(buf, needle) != NULL;
}

/* Find a pid whose cmdline contains any of the needles AND passes ok().
 * ok() may stash side effects (e.g. the opened mapping fd). Throttled:
 * /proc walking at 250 Hz would be wasteful while the game is closed. */
static int find_pid_any(const char *const *needles, double *last_scan, double throttle,
                        int (*ok)(int)) {
    double now = games_now();
    DIR *d;
    struct dirent *de;
    size_t k;
    if (!needles[0]) return -1;
    if (now - *last_scan < throttle) return -1;
    *last_scan = now;
    d = opendir("/proc");
    if (!d) return -1;
    while ((de = readdir(d))) {
        int pid;
        if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
        pid = atoi(de->d_name);
        if (pid == getpid()) continue;
        for (k = 0; needles[k]; k++) {
            if (pid_matches_cmdline(pid, needles[k]) && (!ok || ok(pid))) {
                closedir(d);
                return pid;
            }
        }
    }
    closedir(d);
    return -1;
}

typedef struct {
    size_t min_size, max_size;
    int (*validate)(const unsigned char *head, ssize_t n);
} map_filter;

/* Open one of pid's wine-mapping fds whose size fits [min,max] and whose
 * first bytes pass validate(). Returns read-only fd or -1. */
static int open_wine_mapping_fd(int pid, const map_filter *f) {
    char fdpath[64], full[320], ln[256];
    DIR *d;
    struct dirent *de;
    int best = -1;
    unsigned char head[512];
    if (pid <= 0 || !f) return -1;
    snprintf(fdpath, sizeof(fdpath), "/proc/%d/fd", pid);
    d = opendir(fdpath);
    if (!d) return -1;
    while ((de = readdir(d))) {
        ssize_t n;
        struct stat st;
        int fd;
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        snprintf(full, sizeof(full), "%s/%s", fdpath, de->d_name);
        n = readlink(full, ln, sizeof(ln) - 1);
        if (n < 0) continue;
        ln[n] = 0;
        if (!strstr(ln, "wine-mapping")) continue;
        fd = open(full, O_RDONLY);
        if (fd < 0) continue;
        if (fstat(fd, &st) != 0 ||
            st.st_size < (off_t)f->min_size || st.st_size > (off_t)f->max_size) {
            close(fd);
            continue;
        }
        memset(head, 0, sizeof(head));
        n = pread(fd, head, sizeof(head), 0);
        if (n <= 0 || !f->validate(head, n)) {
            close(fd);
            continue;
        }
        if (best >= 0) close(best);
        best = fd;
    }
    closedir(d);
    return best;
}

/* ------------------------------------------------------------------ */
/* RaceRoom — native $R3E shared memory (API v3.x)                    */

#define R3E_PREFIX_MAX 16384
/* ------------------------------------------------------------------ */

static int s_r3e_fd = -1;
static int s_r3e_logged = 0;
static double s_r3e_last_scan = 0;
static double s_r3e_simt = 0;
static double s_r3e_simt_t = 0;
static int s_r3e_simt_inited = 0;
static dgate s_r3e_g;

static int r3e_validate(const unsigned char *head, ssize_t n) {
    int32_t major, minor, all_off, drv_sz;
    if (n < 16) return 0;
    memcpy(&major, head + 0, 4);
    memcpy(&minor, head + 4, 4);
    memcpy(&all_off, head + 8, 4);
    memcpy(&drv_sz, head + 12, 4);
    if (major != R3E_VERSION_MAJOR) return 0;
    if (minor < 0 || minor > 99) return 0;
    if (all_off < 1024 || all_off > 131072) return 0;
    if (drv_sz < 64 || drv_sz > 16384) return 0;
    return 1;
}

/* Attach candidate: open the $R3E mapping of this pid; keep the fd. */
static int r3e_try_attach(int pid) {
    map_filter f;
    int fd;
    f.min_size = 32768;
    f.max_size = 262144;
    f.validate = r3e_validate;
    fd = open_wine_mapping_fd(pid, &f);
    if (fd < 0) return 0; /* host-side process (reaper/proton/...) mentioning the exe */
    s_r3e_fd = fd;
    return 1;
}

int sc_r3e_read(ScTelem *out) {
    static const char *const needles[] = { "rrre", NULL };
    unsigned char buf[R3E_PREFIX_MAX];
    ssize_t n;
    double simt;
    size_t need;

    if (s_r3e_fd < 0) {
        /* Many host processes (reaper, proton, bwrap...) carry "rrre" in
         * their cmdline; only the game has the $R3E mapping, so candidates
         * are validated via r3e_try_attach inside the scan. */
        if (find_pid_any(needles, &s_r3e_last_scan, 1.0, r3e_try_attach) <= 0)
            return -1;
        s_r3e_simt_inited = 0;
        if (!s_r3e_logged) {
            fprintf(stderr, "\nsimcontrol: RaceRoom shared memory attached\n");
            s_r3e_logged = 1;
        }
    }

    need = offsetof(struct r3e_share, control_type) + 64;
    if (need > sizeof(buf)) need = sizeof(buf);
    memset(buf, 0, sizeof(buf));
    n = pread(s_r3e_fd, buf, need, 0);
    if (n < (ssize_t)need) {
        close(s_r3e_fd);
        s_r3e_fd = -1;
        return -1;
    }

    /* Frozen-mapping watchdog: game_simulation_time stops when the mapping
     * is stale (game closed it / moved it to another fd). */
    memcpy(&simt, buf + offsetof(struct r3e_share, player.game_simulation_time), 8);
    if (!isfinite(simt)) simt = 0;
    if (!s_r3e_simt_inited || simt != s_r3e_simt) {
        s_r3e_simt = simt;
        s_r3e_simt_t = games_now();
        s_r3e_simt_inited = 1;
    } else if (games_now() - s_r3e_simt_t > 3.0) {
        close(s_r3e_fd);
        s_r3e_fd = -1;
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->connected = 1;
    out->src = SC_SRC_R3E;
    {
        int32_t menus = 0, mode = 0, phase = 0, minor = 0;
        memcpy(&menus, buf + offsetof(struct r3e_share, game_in_menus), 4);
        memcpy(&mode, buf + offsetof(struct r3e_share, game_mode), 4);
        memcpy(&phase, buf + offsetof(struct r3e_share, session_phase), 4);
        memcpy(&minor, buf + offsetof(struct r3e_share, version_minor), 4);
        out->version = minor;
        out->playing = !menus && mode >= 0 && phase >= 2 && phase <= 6;
    }
    {
        double vx = 0, vy = 0, vz = 0, yaw = 0;
        float speed = 0, rps = -1;
        memcpy(&vx, buf + offsetof(struct r3e_share, player.local_velocity.x), 8);
        memcpy(&vy, buf + offsetof(struct r3e_share, player.local_velocity.y), 8);
        memcpy(&vz, buf + offsetof(struct r3e_share, player.local_velocity.z), 8);
        memcpy(&yaw, buf + offsetof(struct r3e_share, player.local_angular_velocity.y), 8);
        memcpy(&speed, buf + offsetof(struct r3e_share, car_speed), 4);
        memcpy(&rps, buf + offsetof(struct r3e_share, engine_rps), 4);
        if (!vel_sane((float)vx, (float)vy, (float)vz, (float)yaw, speed))
            return -1;   /* torn sample; keep fd, retry next tick */
        if (!delta_sane(&s_r3e_g, (float)vx, (float)vy, (float)vz, (float)yaw))
            return -1;   /* impossible jump vs last accepted sample */
        out->local_vx = sc_number_guard((float)vx, 0.f);
        out->local_vy = sc_number_guard((float)vy, 0.f);
        out->local_vz = sc_number_guard((float)vz, 0.f);
        out->ang_y = sc_number_guard((float)yaw, 0.f);
        out->speed = sc_number_guard(speed, 0.f);
        out->rpm = rps > 0 ? sc_number_guard(rps * 9.5493f, 0.f) : 0.f;
    }
    {
        /* tire_load: -1 = N/A */
        float load[4];
        memcpy(load, buf + offsetof(struct r3e_share, tire_load), sizeof(load));
        if (load[0] < 0 || load[1] < 0) out->front_grounded = 1;
        else out->front_grounded = (load[0] + load[1] > 50.f);
        if (load[2] < 0 || load[3] < 0) out->rear_grounded = 1;
        else out->rear_grounded = (load[2] + load[3] > 50.f);
    }
    copy_printable(out->car, sizeof(out->car),
                   buf + offsetof(struct r3e_share, vehicle_info.name), 63);
    copy_printable(out->track, sizeof(out->track),
                   buf + offsetof(struct r3e_share, track_name), 63);
    if (!out->car[0]) snprintf(out->car, sizeof(out->car), "RaceRoom");
    return 0;
}

/* ------------------------------------------------------------------ */
/* AMS1 / rF1 — "$rFactorShared$" via community plugin DLL            */
/* ------------------------------------------------------------------ */

static int s_rf1_fd = -1;
static int s_rf1_logged = 0;
static double s_rf1_last_scan = 0;
static unsigned char s_rf1_hb[256];
static int s_rf1_hb_valid = 0;
static double s_rf1_hb_t = 0;
static dgate s_rf1_g;

/* Empirical layout of the shipped plugin build (Dan Allongo original;
 * differs from rfdata.h — no version string, player name first):
 *   deltaTime f @0x00 | lapNumber i @0x04 | lapStartET f @0x08
 *   playerName[64] @0x0c | trackName[64] @0x4c
 *   pos[3] @0x8c | localVel[3] @0x98 (+x left, +z back)
 *   localAccel[3] @0xa4 | ori rows @0xb0 (row Y .y ~ +1 upright)
 *   localRot[3] @0xd4 (+y yaw RIGHT) | speed @0xec | gear @0xf0
 *   engineRPM @0xf4 | unfilteredThrottle @0x104 | steering @0x10c     */

#define RF1_VX      0x98
#define RF1_VZ      0xa0
#define RF1_YAWY    0xd8
#define RF1_SPEED   0xec
#define RF1_RPM     0xf4

static int rf1_validate(const unsigned char *head, ssize_t n) {
    float dt, spd, upy, rpm;
    if (n < 0x120) return 0;
    memcpy(&dt, head + 0, 4);
    memcpy(&upy, head + 0xc0, 4);
    memcpy(&spd, head + RF1_SPEED, 4);
    memcpy(&rpm, head + RF1_RPM, 4);
    if (!isfinite(dt) || dt < -0.01f || dt > 0.5f) return 0;
    /* orientation row Y should be near unit magnitude */
    if (!isfinite(upy) || upy < -1.3f || upy > 1.3f || upy == 0.f) return 0;
    if (!isfinite(spd) || spd < -5.f || spd > 250.f) return 0;
    if (!isfinite(rpm) || rpm < 0.f || rpm > 30000.f) return 0;
    {
        int run = 0, i, ok = 0;
        for (i = 0; i < 40; i++) {
            unsigned char c = head[0x0c + i];
            if (c >= 32 && c < 127) { run++; if (run >= 3) { ok = 1; break; } }
            else run = 0;
        }
        if (!ok) return 0;   /* player name must be printable */
    }
    return 1;
}

static int rf1_try_attach(int pid) {
    map_filter f;
    int fd;
    f.min_size = 8192;
    f.max_size = 65536;
    f.validate = rf1_validate;
    fd = open_wine_mapping_fd(pid, &f);
    if (fd < 0) return 0;
    s_rf1_fd = fd;
    return 1;
}

int sc_rf1_read(ScTelem *out) {
    static const char *const needles[] = { "ams.exe", "automobilista.exe", "rfactor.exe", NULL };
    unsigned char buf[256];
    ssize_t n;
    float vx, vy, vz, yaw, spd;

    if (s_rf1_fd < 0) {
        if (find_pid_any(needles, &s_rf1_last_scan, 1.0, rf1_try_attach) <= 0)
            return -1;
        s_rf1_hb_valid = 0;
        s_rf1_g.have = 0;
        if (!s_rf1_logged) {
            fprintf(stderr, "\nsimcontrol: AMS1 shared memory attached ($rFactorShared$)\n");
            s_rf1_logged = 1;
        }
    }

    memset(buf, 0, sizeof(buf));
    n = pread(s_rf1_fd, buf, sizeof(buf), 0);
    if (n < (ssize_t)sizeof(buf)) {
        close(s_rf1_fd);
        s_rf1_fd = -1;
        return -1;
    }
    /* Heartbeat: frozen prefix outside a session means stale/quit. */
    if (!s_rf1_hb_valid || memcmp(buf, s_rf1_hb, sizeof(buf)) != 0) {
        memcpy(s_rf1_hb, buf, sizeof(buf));
        s_rf1_hb_valid = 1;
        s_rf1_hb_t = games_now();
    } else if (games_now() - s_rf1_hb_t > 2.0) {
        close(s_rf1_fd);
        s_rf1_fd = -1;
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->connected = 1;
    out->src = SC_SRC_RF1;
    out->playing = 1;

    memcpy(&vx, buf + RF1_VX, 4);      /* +x left */
    memcpy(&vy, buf + RF1_VX + 4, 4);
    memcpy(&vz, buf + RF1_VZ, 4);      /* +z back */
    memcpy(&yaw, buf + RF1_YAWY, 4);   /* documented +y yaw right */
    memcpy(&spd, buf + RF1_SPEED, 4);
    if (!vel_sane(vx, vy, vz, yaw, spd)) {
        /* bytes are fresh, just a torn sample — refresh heartbeat, skip */
        s_rf1_hb_t = games_now();
        return -1;
    }
    if (!delta_sane(&s_rf1_g, vx, vy, vz, yaw))
        return -1;   /* impossible jump vs last accepted sample */
    out->local_vx = sc_number_guard(vx, 0.f);
    out->local_vy = sc_number_guard(vy, 0.f);
    out->local_vz = sc_number_guard(vz, 0.f);
    /* Pipeline expects left turn = +yaw; negate the right-positive axis. */
    out->ang_y = sc_number_guard(-yaw, 0.f);
    out->speed = sc_number_guard(spd < 0.f ? sqrtf(vx * vx + vz * vz) : spd, 0.f);
    {
        float rpm;
        memcpy(&rpm, buf + RF1_RPM, 4);
        out->rpm = sc_number_guard(rpm, 0.f);
    }
    out->front_grounded = 1;
    out->rear_grounded = 1;
    copy_printable(out->track, sizeof(out->track), buf + 0x4c, 63);
    snprintf(out->car, sizeof(out->car), "Automobilista");
    return 0;
}

/* ------------------------------------------------------------------ */
/* rFactor 2 — telemetry map via TheIronWolf's plugin                 */
/* ------------------------------------------------------------------ */

#define RF2_HDR 16              /* versionBegin/End, hint, numVehicles */
#define RF2_VEH_STRIDE 1904     /* sizeof(rF2VehicleTelemetry), pack 4 */

static int s_rf2_fd = -1;
static int s_rf2_logged = 0;
static double s_rf2_last_scan = 0;
static double s_rf2_elapsed = 0;
static double s_rf2_elapsed_t = 0;
static int s_rf2_elapsed_inited = 0;
static dgate s_rf2_g;

/* Vehicle entry offsets (pack 4), relative to vehicles[i]:
 *   mDeltaTime @4 | mElapsedTime @12 | mVehicleName[64] @32
 *   mTrackName[64] @96 | mLocalVel @184 | mLocalRot @304 (+y right)
 *   mEngineRPM @356                                                    */

static int rf2_validate(const unsigned char *head, ssize_t n) {
    int32_t vb, ve, nv;
    double dte;
    if (n < RF2_HDR + 128) return 0;
    memcpy(&vb, head + 0, 4);
    memcpy(&ve, head + 4, 4);
    if (vb <= 0 || vb != ve) return 0;   /* consistent double buffer */
    memcpy(&nv, head + 12, 4);
    if (nv < 0 || nv > 128) return 0;
    if (!isprint(head[RF2_HDR + 32])) return 0;
    memcpy(&dte, head + RF2_HDR + 4, 8);
    if (!isfinite(dte) || dte <= 0 || dte > 0.5) return 0;
    return 1;
}

static int rf2_try_attach(int pid) {
    map_filter f;
    int fd;
    f.min_size = 65536;
    f.max_size = 262144;
    f.validate = rf2_validate;
    fd = open_wine_mapping_fd(pid, &f);
    if (fd < 0) return 0;
    s_rf2_fd = fd;
    return 1;
}

int sc_rf2_read(ScTelem *out) {
    static const char *const needles[] = { "rfactor2.exe", NULL };
    unsigned char buf[512];
    ssize_t n;
    int32_t vb, ve;
    double elapsed;

    if (s_rf2_fd < 0) {
        if (find_pid_any(needles, &s_rf2_last_scan, 1.0, rf2_try_attach) <= 0)
            return -1;
        s_rf2_elapsed_inited = 0;
        s_rf2_g.have = 0;
        if (!s_rf2_logged) {
            fprintf(stderr, "\nsimcontrol: rFactor 2 telemetry attached\n");
            s_rf2_logged = 1;
        }
    }

    memset(buf, 0, sizeof(buf));
    n = pread(s_rf2_fd, buf, sizeof(buf), 0);
    if (n < (ssize_t)sizeof(buf)) {
        close(s_rf2_fd);
        s_rf2_fd = -1;
        return -1;
    }
    memcpy(&vb, buf + 0, 4);
    memcpy(&ve, buf + 4, 4);
    if (vb != ve) return -1;   /* mid-update; retry next tick, keep fd */

    memcpy(&elapsed, buf + RF2_HDR + 12, 8);
    if (!s_rf2_elapsed_inited || elapsed != s_rf2_elapsed) {
        s_rf2_elapsed = elapsed;
        s_rf2_elapsed_t = games_now();
        s_rf2_elapsed_inited = 1;
    } else if (games_now() - s_rf2_elapsed_t > 1.5) {
        close(s_rf2_fd);
        s_rf2_fd = -1;
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->connected = 1;
    out->src = SC_SRC_RF2;
    out->playing = 1;   /* fresh elapsed only reaches this point */
    {
        double vx = 0, vy = 0, vz = 0, yaw = 0, rpm = 0;
        memcpy(&vx, buf + RF2_HDR + 184, 8);       /* +x left */
        memcpy(&vy, buf + RF2_HDR + 192, 8);
        memcpy(&vz, buf + RF2_HDR + 200, 8);       /* +z back */
        memcpy(&yaw, buf + RF2_HDR + 312, 8);      /* localRot.y */
        memcpy(&rpm, buf + RF2_HDR + 356, 8);
        if (!vel_sane((float)vx, (float)vy, (float)vz, (float)yaw,
                      sqrtf((float)vx * (float)vx + (float)vz * (float)vz)))
            return -1;   /* torn sample; keep fd, retry next tick */
        if (!delta_sane(&s_rf2_g, (float)vx, (float)vy, (float)vz, (float)yaw))
            return -1;   /* impossible jump vs last accepted sample */
        out->local_vx = sc_number_guard((float)vx, 0.f);
        out->local_vy = sc_number_guard((float)vy, 0.f);
        out->local_vz = sc_number_guard((float)vz, 0.f);
        out->ang_y = sc_number_guard(-(float)yaw, 0.f);
        out->speed = sqrtf(out->local_vx * out->local_vx +
                           out->local_vz * out->local_vz);
        out->rpm = rpm > 0 ? sc_number_guard((float)rpm, 0.f) : 0.f;
    }
    out->front_grounded = 1;
    out->rear_grounded = 1;
    copy_printable(out->car, sizeof(out->car), buf + RF2_HDR + 32, 63);
    copy_printable(out->track, sizeof(out->track), buf + RF2_HDR + 96, 63);
    return 0;
}
