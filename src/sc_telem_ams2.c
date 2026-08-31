#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sc_telem.h"
#include "sc_math.h"
#include "pcars2data.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef TYRE_IS_ON_GROUND
#define TYRE_IS_ON_GROUND (1u << 2)
#endif

static double telem_now(void);

/* Packed offsets from SMS UDP car-physics packet (type 0). */
#define UDP_OFF_TYPE       10
#define UDP_OFF_SPEED      36
#define UDP_OFF_LOCALVEL   64
#define UDP_OFF_ANGVEL     88
#define UDP_OFF_TYREFLAGS  136
#define UDP_PHYS_MIN       140

struct ScTelemSrc {
    int fd;
    size_t size;
    void *map;
    char path[256];
    int using_shm_open;
    int udp_fd;
    int udp_port;
    int udp_logged;
    ScTelem udp_snap;
    int udp_have;
    int udp_game_state; /* -1 unknown, else SMS GameState enum */
    double udp_last_t;
    int memfd;
    int memfd_pid;
    int memfd_src;
    int memfd_logged;

    int ace_fd;       /* graphics: status + (Evo) car name */
    int ace_phys_fd;  /* SPageFilePhysics: localVelocity / yaw */
    int ace_stat_fd;
    int ace_pid;
    int ace_src;      /* SC_SRC_ACEVO or SC_SRC_ACRALLY */
    int ace_logged;
    unsigned ace_last_pkt;   /* last physics packet id seen, to detect a stale/frozen mapping */
    double   ace_last_pkt_t; /* when we last saw the packet id change */
    int      ace_pkt_inited;
};

/*
 * AC-family wine mappings (Evo + Rally, GameThread):
 *   physics  — SPageFilePhysics (800 B struct, mapping 800–4096)
 *   graphics — packetId + status (2=LIVE); Evo is 8KiB, Rally ~2KiB
 *   static   — Evo ASCII "1.0"; Rally/AC wchar smVersion
 * Do not use acbridge.exe / extra Proton. Graphics +220 is not velocity.
 */
#define ACE_OFF_PACKET     0
#define ACE_OFF_STATUS     4
#define ACE_OFF_CAR        3086
#define ACE_STAT_OFF_TRACK 136
#define AC_STAT_OFF_CAR_U16   68
#define AC_STAT_OFF_TRACK_U16 134
#define ACE_LIVE           2

#define ACE_PHYS_GAS       4
#define ACE_PHYS_BRAKE     8
#define ACE_PHYS_FUEL      12
#define ACE_PHYS_RPMS      20
#define ACE_PHYS_STEER     24
#define ACE_PHYS_SPEED     28
#define ACE_PHYS_WLOAD     72
#define ACE_PHYS_HEADING   208
#define ACE_PHYS_ANGVEL    296
#define ACE_PHYS_LVEL      568
#define ACE_PHYS_MIN       580

static int ac_kind_from_pid(int pid)
{
    char path[64], buf[1024];
    ssize_t n;
    int fd, i;
    if (pid <= 0) return 0;
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
    if (strcasestr(buf, "AssettoCorsaEVO.exe")) return SC_SRC_ACEVO;
    if (strcasestr(buf, "acr.exe") || strcasestr(buf, "Assetto Corsa Rally"))
        return SC_SRC_ACRALLY;
    return 0;
}

static int ace_phys_plausible(const unsigned char *p, size_t n);

static int open_wine_ace_live_fd(int pid)
{
    char fdpath[128], link[256];
    DIR *d;
    struct dirent *de;
    int best = -1;
    unsigned best_pkt = 0;
    if (pid <= 0) return -1;
    snprintf(fdpath, sizeof(fdpath), "/proc/%d/fd", pid);
    d = opendir(fdpath);
    if (!d) return -1;
    while ((de = readdir(d))) {
        char full[416];
        ssize_t n;
        struct stat st;
        unsigned hdr[2];
        int fd;
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        snprintf(full, sizeof(full), "%s/%s", fdpath, de->d_name);
        n = readlink(full, link, sizeof(link) - 1);
        if (n < 0) continue;
        link[n] = 0;
        if (!strstr(link, "wine-mapping")) continue;
        fd = open(full, O_RDONLY);
        if (fd < 0) continue;
        /* Evo graphics is 8KiB; Rally/AC is ~2KiB. Skip physics-sized pages
         * that look like SPageFilePhysics (gas/speed), not status. */
        if (fstat(fd, &st) != 0 || st.st_size < 512 || st.st_size > 16384) {
            close(fd);
            continue;
        }
        if (pread(fd, hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) {
            close(fd);
            continue;
        }
        {
            unsigned char head[32];
            memset(head, 0, sizeof(head));
            memcpy(head, hdr, sizeof(hdr));
            if (st.st_size >= 32)
                pread(fd, head, sizeof(head), 0);
            if (ace_phys_plausible(head, sizeof(head))) {
                close(fd);
                continue;
            }
        }
        /* packetId + AC_STATUS 0..3 (2=on track) */
        if (hdr[1] <= 3 && hdr[0] > 10 && hdr[0] < 500000u) {
            if ((int)hdr[0] >= (int)best_pkt) {
                if (best >= 0) close(best);
                best = fd;
                best_pkt = hdr[0];
                continue;
            }
        }
        close(fd);
    }
    closedir(d);
    return best;
}

static int open_wine_ace_static_fd(int pid)
{
    char fdpath[128], link[256], buf[160];
    DIR *d;
    struct dirent *de;
    if (pid <= 0) return -1;
    snprintf(fdpath, sizeof(fdpath), "/proc/%d/fd", pid);
    d = opendir(fdpath);
    if (!d) return -1;
    while ((de = readdir(d))) {
        char full[416];
        ssize_t n;
        struct stat st;
        int fd;
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        snprintf(full, sizeof(full), "%s/%s", fdpath, de->d_name);
        n = readlink(full, link, sizeof(link) - 1);
        if (n < 0) continue;
        link[n] = 0;
        if (!strstr(link, "wine-mapping")) continue;
        fd = open(full, O_RDONLY);
        if (fd < 0) continue;
        if (fstat(fd, &st) != 0 || st.st_size < 512 || st.st_size > 4096) {
            close(fd);
            continue;
        }
        memset(buf, 0, sizeof(buf));
        if (pread(fd, buf, sizeof(buf) - 1, 0) < 140) {
            close(fd);
            continue;
        }
        /* Evo ASCII "1.0"; Rally/AC wchar carModel at +68 (smVersion may be empty). */
        {
            int carlen = 0;
            while (carlen < 32) {
                unsigned c = (unsigned char)buf[68 + carlen * 2]
                           | ((unsigned)(unsigned char)buf[69 + carlen * 2] << 8);
                if (c < 32 || c > 126) break;
                carlen++;
            }
            if (!memcmp(buf, "1.0", 3) || strstr(buf, "0.8.1") ||
                (buf[0] >= '0' && buf[0] <= '9' && buf[1] == 0 && buf[2] == '.') ||
                carlen >= 3) {
                closedir(d);
                return fd;
            }
        }
        close(fd);
    }
    closedir(d);
    return -1;
}

static int ace_phys_plausible(const unsigned char *p, size_t n)
{
    unsigned pkt;
    int rpms;
    float gas, brake, fuel, speed;
    if (n < 32) return 0;
    /* static page starts with smVersion "1.0" */
    if (p[0] >= '0' && p[0] <= '9' && p[1] == '.') return 0;
    memcpy(&pkt, p + ACE_OFF_PACKET, 4);
    memcpy(&gas, p + ACE_PHYS_GAS, 4);
    memcpy(&brake, p + ACE_PHYS_BRAKE, 4);
    memcpy(&fuel, p + ACE_PHYS_FUEL, 4);
    memcpy(&rpms, p + ACE_PHYS_RPMS, 4);
    memcpy(&speed, p + ACE_PHYS_SPEED, 4);
    if (pkt < 10u || pkt > 50000000u) return 0;
    if (!isfinite(gas) || !isfinite(brake) || !isfinite(fuel) || !isfinite(speed))
        return 0;
    if (gas < -0.05f || gas > 1.2f) return 0;
    if (brake < -0.05f || brake > 1.2f) return 0;
    if (fuel < 0.f || fuel > 500.f) return 0;
    if (speed < 0.f || speed > 500.f) return 0;
    if (rpms < 0 || rpms > 25000) return 0;
    return 1;
}

static int open_wine_ace_phys_fd(int pid)
{
    char fdpath[128], link[256];
    unsigned char head[32];
    DIR *d;
    struct dirent *de;
    /* Liveness beats packet magnitude: a stale mapping from a previous
     * game generation (kept alive by wineserver as deleted-but-open) can
     * carry a MUCH higher frozen packet counter than the live one, and
     * best-pkt selection then latches the ghost — its watchdog evicts it,
     * the next scan re-selects it, and the status flaps WAIT/ON. Sample
     * every candidate twice ~120 ms apart; only ADVANCING packet ids are
     * eligible. */
#define ACE_PHYS_CAND_MAX 8
    struct { int fd; unsigned pkt0; } cand[ACE_PHYS_CAND_MAX];
    int ncand = 0;
    int best = -1;
    unsigned best_pkt = 0;
    if (pid <= 0) return -1;
    snprintf(fdpath, sizeof(fdpath), "/proc/%d/fd", pid);
    d = opendir(fdpath);
    if (!d) return -1;
    while ((de = readdir(d))) {
        char full[416];
        ssize_t n;
        struct stat st;
        unsigned pkt;
        int fd;
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        snprintf(full, sizeof(full), "%s/%s", fdpath, de->d_name);
        n = readlink(full, link, sizeof(link) - 1);
        if (n < 0) continue;
        link[n] = 0;
        if (!strstr(link, "wine-mapping")) continue;
        fd = open(full, O_RDONLY);
        if (fd < 0) continue;
        /* physics is 4KiB; 8KiB is graphics */
        if (fstat(fd, &st) != 0 || st.st_size < ACE_PHYS_MIN || st.st_size > 6000) {
            close(fd);
            continue;
        }
        if (pread(fd, head, sizeof(head), 0) != (ssize_t)sizeof(head) ||
            !ace_phys_plausible(head, sizeof(head))) {
            close(fd);
            continue;
        }
        memcpy(&pkt, head, 4);
        if (ncand < ACE_PHYS_CAND_MAX) {
            cand[ncand].fd = fd;
            cand[ncand].pkt0 = pkt;
            ncand++;
            continue;
        }
        close(fd);
    }
    closedir(d);
    if (ncand > 0)
        usleep(120000);
    for (int i = 0; i < ncand; i++) {
        unsigned pkt1 = 0;
        if (pread(cand[i].fd, head, sizeof(head), 0) == (ssize_t)sizeof(head))
            memcpy(&pkt1, head, 4);
        if (pkt1 != cand[i].pkt0 && pkt1 >= best_pkt) {
            /* advancing: alive. Drop the previous pick. */
            if (best >= 0) close(best);
            best = cand[i].fd;
            best_pkt = pkt1;
            continue;
        }
        close(cand[i].fd);
    }
#undef ACE_PHYS_CAND_MAX
    return best;
}

static void ace_copy_str(char *dst, size_t n, const char *src, size_t maxsrc)
{
    size_t i = 0;
    if (!src || n == 0) return;
    while (i + 1 < n && i < maxsrc && src[i] >= 32 && src[i] < 127) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void ace_copy_utf16(char *dst, size_t n, const unsigned char *src, size_t maxch)
{
    size_t i = 0, o = 0;
    if (!dst || n == 0 || !src) return;
    while (o + 1 < n && i + 1 < maxch * 2) {
        unsigned c = (unsigned)src[i] | ((unsigned)src[i + 1] << 8);
        i += 2;
        if (c == 0) break;
        if (c < 32 || c > 126) continue;
        dst[o++] = (char)c;
    }
    dst[o] = 0;
}

static int scan_acevo_phys(int *out_pid)
{
    DIR *d = opendir("/proc");
    struct dirent *de;
    int fallback_fd = -1, fallback_pid = -1;
    if (!d) return -1;
    while ((de = readdir(d))) {
        char comm[64];
        FILE *cf;
        int pid, fd, skip_pref = 0, kind;
        if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
        pid = atoi(de->d_name);
        if (pid == getpid()) continue;
        kind = ac_kind_from_pid(pid);
        snprintf(comm, sizeof(comm), "/proc/%d/comm", atoi(de->d_name));
        cf = fopen(comm, "r");
        if (cf) {
            if (!fgets(comm, sizeof(comm), cf)) comm[0] = 0;
            fclose(cf);
            comm[strcspn(comm, "\n")] = 0;
            if (!strcmp(comm, "wineserver") || !strcmp(comm, "simcontrol"))
                skip_pref = 1;
        }
        fd = open_wine_ace_phys_fd(pid);
        if (fd < 0) continue;
        /* Known AC exe (Evo / Rally) wins over a random wine mapping. */
        if (kind) {
            if (fallback_fd >= 0) close(fallback_fd);
            *out_pid = pid;
            closedir(d);
            return fd;
        }
        if (skip_pref) {
            if (fallback_fd < 0) {
                fallback_fd = fd;
                fallback_pid = pid;
            } else {
                close(fd);
            }
            continue;
        }
        if (fallback_fd >= 0) close(fallback_fd);
        *out_pid = pid;
        closedir(d);
        return fd;
    }
    closedir(d);
    if (fallback_fd >= 0) {
        *out_pid = fallback_pid;
        return fallback_fd;
    }
    return -1;
}

static void ace_close_maps(ScTelemSrc *t)
{
    if (t->ace_fd >= 0) { close(t->ace_fd); t->ace_fd = -1; }
    if (t->ace_phys_fd >= 0) { close(t->ace_phys_fd); t->ace_phys_fd = -1; }
    if (t->ace_stat_fd >= 0) { close(t->ace_stat_fd); t->ace_stat_fd = -1; }
    t->ace_pid = -1;
    t->ace_src = 0;
    t->ace_pkt_inited = 0;
}

static int memfd_read_acevo(ScTelemSrc *t, ScTelem *out)
{
    unsigned char phys[2048], gfx[8192];
    ssize_t n;
    unsigned packet = 0, status = 0;
    int rpms = 0;
    float speed_kmh, vx, vy, vz, yaw, heading, steer, angx;
    float wload[4];
    /* Physics lives on GameThread's wine-mapping (SPageFilePhysics).
     * Do not match cmdline alone — Proton/reaper also mention the exe. */
    if (t->ace_phys_fd < 0) {
        /* scan_acevo_phys also walks all of /proc — throttle it the same
         * way as the pcars2 discovery (see memfd_read_pcars). */
        static double s_ace_scan_t = -10;
        if (telem_now() - s_ace_scan_t < 1.0) return -1;
        s_ace_scan_t = telem_now();
        int pid = -1;
        int fd = scan_acevo_phys(&pid);
        if (fd < 0) return -1;
        t->ace_phys_fd = fd;
        t->ace_pid = pid;
        t->ace_src = ac_kind_from_pid(pid);
        if (!t->ace_src) t->ace_src = SC_SRC_ACEVO;
        t->ace_fd = open_wine_ace_live_fd(pid);
        t->ace_stat_fd = open_wine_ace_static_fd(pid);
        if (!t->ace_logged) {
            fprintf(stderr, "\nsimcontrol: reading %s physics from pid %d (SPageFilePhysics)\n",
                    t->ace_src == SC_SRC_ACRALLY ? "AC Rally" : "AC Evo", pid);
            t->ace_logged = 1;
        }
    }
    memset(phys, 0, sizeof(phys));
    n = pread(t->ace_phys_fd, phys, sizeof(phys), 0);
    if (n < ACE_PHYS_MIN || !ace_phys_plausible(phys, (size_t)n)) {
        ace_close_maps(t);
        return -1;
    }
    memcpy(&packet, phys + ACE_OFF_PACKET, 4);

    /* The menu can make the game close this wine-mapping and open a new one
     * at another fd; our fd then keeps returning the last frozen bytes
     * forever (still "plausible", just static) instead of failing. Detect
     * that by watching the packet counter: if it stops advancing for a bit
     * while we think we're connected, force a full re-scan so we pick up
     * the game's current mapping instead of feeding stale physics to the
     * assist. */
    {
        double now = telem_now();
        if (!t->ace_pkt_inited || packet != t->ace_last_pkt) {
            t->ace_last_pkt = packet;
            t->ace_last_pkt_t = now;
            t->ace_pkt_inited = 1;
        } else if (now - t->ace_last_pkt_t > 0.75) {
            ace_close_maps(t);
            return -1;
        }
    }

    memcpy(&rpms, phys + ACE_PHYS_RPMS, 4);
    memcpy(&steer, phys + ACE_PHYS_STEER, 4);
    memcpy(&speed_kmh, phys + ACE_PHYS_SPEED, 4);
    memcpy(&heading, phys + ACE_PHYS_HEADING, 4);
    memcpy(&angx, phys + ACE_PHYS_ANGVEL, 4);
    memcpy(&yaw, phys + ACE_PHYS_ANGVEL + 4, 4); /* localAngularVel.y */
    memcpy(&vx, phys + ACE_PHYS_LVEL, 4);
    memcpy(&vy, phys + ACE_PHYS_LVEL + 4, 4);
    memcpy(&vz, phys + ACE_PHYS_LVEL + 8, 4);
    memcpy(wload, phys + ACE_PHYS_WLOAD, sizeof(wload));

    speed_kmh = sc_number_guard(speed_kmh, 0.f);
    vx = sc_number_guard(vx, 0.f);
    vy = sc_number_guard(vy, 0.f);
    vz = sc_number_guard(vz, 0.f);
    yaw = sc_number_guard(yaw, 0.f);
    angx = sc_number_guard(angx, 0.f);
    heading = sc_number_guard(heading, 0.f);
    steer = sc_number_guard(steer, 0.f);

    if (t->ace_fd >= 0) {
        memset(gfx, 0, sizeof(gfx));
        if (pread(t->ace_fd, gfx, sizeof(gfx), 0) >= 8)
            memcpy(&status, gfx + ACE_OFF_STATUS, 4);
        else {
            close(t->ace_fd);
            t->ace_fd = -1;
        }
    }

    memset(out, 0, sizeof(*out));
    out->connected = 1;
    out->src = t->ace_src ? t->ace_src : SC_SRC_ACEVO;
    /* Rally graphics status stays 0 on stage; Evo uses AC_LIVE=2. */
    if (t->ace_src == SC_SRC_ACRALLY)
        out->playing = 1;
    else
        out->playing = (status == ACE_LIVE) || (speed_kmh > 3.f);
    out->version = 1;
    out->seq = packet;
    out->speed = speed_kmh / 3.6f;
    out->rpm = (float)rpms;
    out->local_vx = vx;
    out->local_vy = vy;
    out->local_vz = vz;
    out->ang_x = angx;
    out->ang_y = yaw;
    out->ang_z = heading;
    out->game_steer = steer;
    out->front_grounded = (wload[0] + wload[1] > 50.f) ? 1 : 0;
    out->rear_grounded  = (wload[2] + wload[3] > 50.f) ? 1 : 0;

    if (t->ace_fd >= 0)
        ace_copy_str(out->car, sizeof(out->car), (char *)gfx + ACE_OFF_CAR, 48);
    if (t->ace_stat_fd >= 0) {
        unsigned char st[256];
        memset(st, 0, sizeof(st));
        if (pread(t->ace_stat_fd, st, sizeof(st) - 1, 0) > 16) {
            if (st[0] >= '0' && st[0] <= '9' && st[1] == '.' ) {
                ace_copy_str(out->track, sizeof(out->track),
                             (char *)st + ACE_STAT_OFF_TRACK, 48);
            } else {
                if (!out->car[0])
                    ace_copy_utf16(out->car, sizeof(out->car),
                                   st + AC_STAT_OFF_CAR_U16, 32);
                ace_copy_utf16(out->track, sizeof(out->track),
                               st + AC_STAT_OFF_TRACK_U16, 32);
            }
        }
    }
    if (!out->car[0]) {
        snprintf(out->car, sizeof(out->car), "%s",
                 out->src == SC_SRC_ACRALLY ? "AC Rally" : "AC Evo");
    }
    return 0;
}

static int open_wine_pcars_memfd(int pid);

static int sms_kind_from_pid(int pid)
{
    char path[64], buf[1024], comm[64];
    ssize_t n;
    int fd, i;
    FILE *f;
    if (pid <= 0) return 0;
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = 0;
            for (i = 0; i < (int)n; i++) {
                if (buf[i] == 0) buf[i] = ' ';
            }
            if (strcasestr(buf, "AMS2AVX.exe") || strcasestr(buf, "Automobilista"))
                return SC_SRC_AMS2;
            if (strcasestr(buf, "pCARS2AVX.exe") || strcasestr(buf, "pCARS2.exe") ||
                strcasestr(buf, "Project CARS 2") || strcasestr(buf, "Project CARS2"))
                return SC_SRC_PCARS2;
        }
    }
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    f = fopen(path, "r");
    if (f) {
        if (!fgets(comm, sizeof(comm), f)) comm[0] = 0;
        fclose(f);
        comm[strcspn(comm, "\n")] = 0;
        if (!strcmp(comm, "AMS2AVX.exe") || !strcmp(comm, "AMS2DemoAVX.exe"))
            return SC_SRC_AMS2;
        if (!strcmp(comm, "pCARS2AVX.exe") || !strcmp(comm, "pCARS2.exe") ||
            !strcmp(comm, "PCars2AVX.exe"))
            return SC_SRC_PCARS2;
    }
    return 0;
}

static int find_sms_pid(int *out_kind)
{
    DIR *d = opendir("/proc");
    struct dirent *de;
    int fallback_pid = -1, fallback_kind = 0;
    if (!d) return -1;
    while ((de = readdir(d))) {
        int pid, kind, fd;
        if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
        pid = atoi(de->d_name);
        if (pid == getpid()) continue;
        kind = sms_kind_from_pid(pid);
        if (!kind) continue;
        fd = open_wine_pcars_memfd(pid);
        if (fd < 0) continue;
        close(fd);
        /* Prefer the game exe over Proton/reaper that only mention the path. */
        {
            char comm[64], cpath[64];
            FILE *cf;
            snprintf(cpath, sizeof(cpath), "/proc/%d/comm", pid);
            cf = fopen(cpath, "r");
            if (cf) {
                if (!fgets(comm, sizeof(comm), cf)) comm[0] = 0;
                fclose(cf);
                comm[strcspn(comm, "\n")] = 0;
                if (!strcmp(comm, "wineserver") || !strcmp(comm, "reaper") ||
                    !strcmp(comm, "python3") || !strcmp(comm, "steam.exe") ||
                    !strcmp(comm, "proton")) {
                    if (fallback_pid < 0) {
                        fallback_pid = pid;
                        fallback_kind = kind;
                    }
                    continue;
                }
            }
        }
        *out_kind = kind;
        closedir(d);
        return pid;
    }
    closedir(d);
    if (fallback_pid >= 0) {
        *out_kind = fallback_kind;
        return fallback_pid;
    }
    return -1;
}

/* Live AMS2/PCARS2 telemetry lives in a wineserver memfd named "wine-mapping", not /dev/shm. */
static int open_wine_pcars_memfd(int pid)
{
    char fdpath[128], link[256];
    DIR *d;
    struct dirent *de;
    if (pid <= 0) return -1;
    snprintf(fdpath, sizeof(fdpath), "/proc/%d/fd", pid);
    d = opendir(fdpath);
    if (!d) return -1;
    while ((de = readdir(d))) {
        char full[416];
        ssize_t n;
        struct stat st;
        unsigned hdr[5];
        int fd;
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        snprintf(full, sizeof(full), "%s/%s", fdpath, de->d_name);
        n = readlink(full, link, sizeof(link) - 1);
        if (n < 0) continue;
        link[n] = 0;
        if (!strstr(link, "wine-mapping")) continue;
        fd = open(full, O_RDONLY);
        if (fd < 0) continue;
        if (fstat(fd, &st) != 0 || st.st_size < 16000 || st.st_size > 200000) {
            close(fd);
            continue;
        }
        if (pread(fd, hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) {
            close(fd);
            continue;
        }
        /* mVersion, mBuildVersionNumber, mGameState */
        if (hdr[0] >= 8 && hdr[0] <= 20 && hdr[2] <= 10) {
            closedir(d);
            return fd;
        }
        close(fd);
    }
    closedir(d);
    return -1;
}

static int fill_from_pcars(const struct pcars2APIStruct *s, ScTelem *out, int src)
{
    if (!s || s->mVersion == 0) return -1;
    memset(out, 0, sizeof(*out));
    out->connected = 1;
    out->src = src ? src : SC_SRC_AMS2;
    out->playing = (s->mGameState == GAME_INGAME_PLAYING ||
                    s->mGameState == GAME_INGAME_INMENU_TIME_TICKING ||
                    s->mGameState == GAME_INGAME_RESTARTING);
    out->version = (int)s->mVersion;
    out->seq = s->mSequenceNumber;
    out->speed = sc_number_guard(s->mSpeed, 0.f);
    out->rpm = sc_number_guard(s->mRpm, 0.f);
    out->local_vx = sc_number_guard(s->mLocalVelocity[VEC_X], 0.f);
    out->local_vy = sc_number_guard(s->mLocalVelocity[VEC_Y], 0.f);
    out->local_vz = sc_number_guard(s->mLocalVelocity[VEC_Z], 0.f);
    out->ang_x = sc_number_guard(s->mAngularVelocity[VEC_X], 0.f);
    out->ang_y = sc_number_guard(s->mAngularVelocity[VEC_Y], 0.f);
    out->ang_z = sc_number_guard(s->mAngularVelocity[VEC_Z], 0.f);
    out->game_steer = sc_number_guard(s->mSteering, 0.f);
    out->front_grounded = ((s->mTyreFlags[TYRE_FRONT_LEFT] | s->mTyreFlags[TYRE_FRONT_RIGHT]) & TYRE_IS_ON_GROUND) ? 1 : 0;
    out->rear_grounded  = ((s->mTyreFlags[TYRE_REAR_LEFT]  | s->mTyreFlags[TYRE_REAR_RIGHT])  & TYRE_IS_ON_GROUND) ? 1 : 0;
    memcpy(out->car, s->mCarName, 63);
    memcpy(out->track, s->mTrackLocation, 63);
    out->car[63] = 0;
    out->track[63] = 0;
    return 0;
}

static int memfd_read_pcars(ScTelemSrc *t, ScTelem *out)
{
    static double s_scan_t = -10;
    struct pcars2APIStruct s;
    ssize_t n;
    /* find_sms_pid walks all of /proc reading cmdlines — far too expensive
     * for every 250 Hz tick. Discover/revalidate at most 4x per second and
     * reuse the cached mapping between scans. */
    if (telem_now() - s_scan_t >= 1.0) {
        s_scan_t = telem_now();
        int kind = 0;
        int pid = find_sms_pid(&kind);
        if (pid <= 0) {
            if (t->memfd >= 0) { close(t->memfd); t->memfd = -1; }
            t->memfd_pid = -1;
            t->memfd_src = 0;
            return -1;
        }
        if (t->memfd < 0 || t->memfd_pid != pid) {
            if (t->memfd >= 0) close(t->memfd);
            t->memfd = open_wine_pcars_memfd(pid);
            t->memfd_pid = pid;
            t->memfd_src = kind;
            if (t->memfd >= 0) {
                fprintf(stderr, "\nsimcontrol: reading %s shared memory from process %d (wine memfd)\n",
                        kind == SC_SRC_PCARS2 ? "Project CARS 2" : "AMS2", pid);
                t->memfd_logged = 1;
            }
        }
    }
    if (t->memfd < 0) return -1;
    memset(&s, 0, sizeof(s));
    n = pread(t->memfd, &s, sizeof(s), 0);
    if (n < 7000 || s.mVersion == 0) {
        close(t->memfd);
        t->memfd = -1;
        return -1;
    }
    /* Freeze watchdog: when the game dies the wineserver keeps the
     * mapping alive (deleted-but-open), feeding the last bytes forever.
     * Use mSequenceNumber as the liveness signal: unlike mCurrentTime it
     * advances on every live frame in both AMS2 and PCARS2 (AMS2 v14
     * leaves mCurrentTime pinned at -1, which would trip a perpetual
     * false-positive freeze here). If the sequence stalls past 2.5 s the
     * mapping is stale and must not be trusted. */
    {
        static unsigned int s_last_seq = 0;
        static double s_last_chg_t = 0;
        double now = telem_now();
        if (s.mSequenceNumber != s_last_seq) {
            s_last_seq = s.mSequenceNumber;
            s_last_chg_t = now;
        } else if (now - s_last_chg_t > 2.5) {
            close(t->memfd);
            t->memfd = -1;
            t->memfd_pid = -1;
            t->memfd_src = 0;
            s_last_seq = 0;
            s_last_chg_t = 0;
            return -1;
        }
    }
    return fill_from_pcars(&s, out, t->memfd_src);
}

static double telem_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int udp_open(ScTelemSrc *t, int port) {
    if (port <= 0) return -1;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "simcontrol: UDP bind :%d failed (%s) — enable AMS2 UDP or pick a free udp_port\n",
                port, strerror(errno));
        close(fd);
        return -1;
    }
    t->udp_fd = fd;
    t->udp_port = port;
    t->udp_game_state = -1;
    fprintf(stderr, "simcontrol: listening for AMS2 UDP on 0.0.0.0:%d\n", port);
    fprintf(stderr, "     AMS2: Options → System → UDP Frequency = not Off\n");
    return 0;
}

static void udp_poll(ScTelemSrc *t) {
    if (t->udp_fd < 0) return;
    unsigned char buf[2048];
    for (;;) {
        ssize_t n = recv(t->udp_fd, buf, sizeof(buf), 0);
        if (n < 12) break;
        unsigned type = buf[UDP_OFF_TYPE];
        if (type == 0 && n >= UDP_PHYS_MIN) {
            float speed, vel[3], ang[3];
            memcpy(&speed, buf + UDP_OFF_SPEED, 4);
            memcpy(vel, buf + UDP_OFF_LOCALVEL, 12);
            memcpy(ang, buf + UDP_OFF_ANGVEL, 12);
            t->udp_snap.connected = 1;
            t->udp_snap.speed = sc_number_guard(speed, 0.f);
            t->udp_snap.local_vx = sc_number_guard(vel[0], 0.f);
            t->udp_snap.local_vy = sc_number_guard(vel[1], 0.f);
            t->udp_snap.local_vz = sc_number_guard(vel[2], 0.f);
            t->udp_snap.ang_x = sc_number_guard(ang[0], 0.f);
            t->udp_snap.ang_y = sc_number_guard(ang[1], 0.f);
            t->udp_snap.ang_z = sc_number_guard(ang[2], 0.f);
            t->udp_snap.front_grounded = ((buf[UDP_OFF_TYREFLAGS] | buf[UDP_OFF_TYREFLAGS + 1]) & TYRE_IS_ON_GROUND) ? 1 : 0;
            t->udp_snap.rear_grounded  = ((buf[UDP_OFF_TYREFLAGS + 2] | buf[UDP_OFF_TYREFLAGS + 3]) & TYRE_IS_ON_GROUND) ? 1 : 0;
            if (!t->udp_snap.car[0]) snprintf(t->udp_snap.car, sizeof(t->udp_snap.car), "AMS2");
            t->udp_have = 1;
            t->udp_last_t = telem_now();
            if (!t->udp_logged) {
                fprintf(stderr, "\nsimcontrol: AMS2 UDP telemetry is live (speed %.1f km/h)\n", t->udp_snap.speed * 3.6f);
                t->udp_logged = 1;
            }
        } else if (type == 4 && n >= 15) {
            t->udp_game_state = (int)(buf[14] & 0x0F);
        }
    }
}

static int try_open_path(ScTelemSrc *t, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(struct pcars2APIStruct)) {
        close(fd);
        return -1;
    }
    size_t sz = (size_t)st.st_size;
    void *m = mmap(NULL, sz, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        close(fd);
        return -1;
    }
    t->fd = fd;
    t->size = sz;
    t->map = m;
    snprintf(t->path, sizeof(t->path), "%s", path);
    t->using_shm_open = 0;
    return 0;
}

static int try_shm_name(ScTelemSrc *t, const char *name) {
    int fd = shm_open(name, O_RDONLY, 0);
    if (fd < 0) return -1;
    size_t sz = (size_t)PCARS2_SIZE;
    if (sz < sizeof(struct pcars2APIStruct)) sz = sizeof(struct pcars2APIStruct);
    void *m = mmap(NULL, sz, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        /* retry with sizeof */
        sz = sizeof(struct pcars2APIStruct);
        m = mmap(NULL, sz, PROT_READ, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) {
            close(fd);
            return -1;
        }
    }
    t->fd = fd;
    t->size = sz;
    t->map = m;
    snprintf(t->path, sizeof(t->path), "shm:%s", name);
    t->using_shm_open = 1;
    return 0;
}

static int telem_try_attach(ScTelemSrc *t, const ScConfig *cfg) {
    const char *candidates_path[] = {
        cfg && cfg->shm_path[0] ? cfg->shm_path : NULL,
        "/dev/shm/$pcars2$",
        "/dev/shm/$pcars2",
        NULL
    };
    const char *candidates_shm[] = {
        "/$pcars2$",
        "$pcars2$",
        "/$pcars2",
        "$pcars2",
        NULL
    };

    for (int i = 0; candidates_path[i]; i++) {
        if (!candidates_path[i] || !candidates_path[i][0]) continue;
        if (try_open_path(t, candidates_path[i]) == 0) return 0;
    }
    for (int i = 0; candidates_shm[i]; i++) {
        if (try_shm_name(t, candidates_shm[i]) == 0) return 0;
    }
    return -1;
}

ScTelemSrc *sc_telem_open(const ScConfig *cfg) {
    ScTelemSrc *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->fd = -1;
    t->udp_fd = -1;
    t->udp_game_state = -1;
    t->memfd = -1;
    t->memfd_pid = -1;
    t->memfd_src = 0;
    t->ace_fd = -1;
    t->ace_phys_fd = -1;
    t->ace_stat_fd = -1;
    t->ace_pid = -1;
    t->ace_src = 0;

    if (telem_try_attach(t, cfg) == 0) {
        fprintf(stderr, "simcontrol: AMS2 SHM at %s (%zu bytes, struct %zu)\n",
                t->path, t->size, sizeof(struct pcars2APIStruct));
        fprintf(stderr, "     (empty until the Wine bridge owns the mapping *before* AMS2 starts)\n");
    } else {
        fprintf(stderr, "simcontrol: no AMS2 SHM yet; UDP telemetry will be used if enabled in-game\n");
    }
    udp_open(t, cfg ? cfg->udp_port : 5606);
    return t;
}

void sc_telem_close(ScTelemSrc *t) {
    if (!t) return;
    if (t->map && t->map != MAP_FAILED) munmap(t->map, t->size);
    if (t->fd >= 0) close(t->fd);
    if (t->udp_fd >= 0) close(t->udp_fd);
    if (t->memfd >= 0) close(t->memfd);
    if (t->ace_fd >= 0) close(t->ace_fd);
    if (t->ace_phys_fd >= 0) close(t->ace_phys_fd);
    if (t->ace_stat_fd >= 0) close(t->ace_stat_fd);
    free(t);
}

static int copy_consistent(ScTelemSrc *t, struct pcars2APIStruct *dst) {
    volatile struct pcars2APIStruct *src = t->map;
    if (!src) return -1;
    for (int i = 0; i < 8; i++) {
        unsigned s1 = src->mSequenceNumber;
        memcpy(dst, (void *)src, sizeof(*dst));
        unsigned s2 = src->mSequenceNumber;
        if (s1 == s2 && (s1 % 2u) == 0u) return 0;
    }
    /* torn; still use last memcpy */
    return 1;
}

/* Sticky-reader fast path: while one source delivers data, ONLY that
 * source is polled. Discovery scans (full /proc walks, ~300 ms with a
 * busy wineserver) then happen exclusively at startup, game switches or
 * source failure — never periodically mid-session, which used to stall
 * the steering loop once per second. */
enum {
  RD_NONE = 0, RD_PCARS_MEMFD, RD_ACEVO, RD_PCARS1,
  RD_R3E, RD_RF1, RD_RF2, RD_LEGACY_MAP, RD_UDP
};

static int sc_telem_read_sources(ScTelemSrc *t, const ScConfig *cfg, ScTelem *out) {
    static int cur_rd = RD_NONE;
    memset(out, 0, sizeof(*out));
    if (!t) return -1;

    if (cur_rd != RD_NONE) {
        int ok = -1;
        switch (cur_rd) {
        case RD_PCARS_MEMFD: ok = memfd_read_pcars(t, out); break;
        case RD_ACEVO:       ok = memfd_read_acevo(t, out); break;
        case RD_PCARS1:      ok = sc_pcars1_read(out); break;
        case RD_R3E:         ok = sc_r3e_read(out); break;
        case RD_RF1:         ok = sc_rf1_read(out); break;
        case RD_RF2:         ok = sc_rf2_read(out); break;
        case RD_LEGACY_MAP:
            if (t->map) {
                struct pcars2APIStruct s;
                memset(&s, 0, sizeof(s));
                if (copy_consistent(t, &s) >= 0 && s.mVersion != 0)
                    ok = fill_from_pcars(&s, out, SC_SRC_AMS2);
            }
            break;
        case RD_UDP:
            udp_poll(t);
            if (t->udp_have && (telem_now() - t->udp_last_t) < 1.5) {
                *out = t->udp_snap;
                if (t->udp_game_state == GAME_INGAME_PLAYING ||
                    t->udp_game_state == GAME_INGAME_INMENU_TIME_TICKING)
                    out->playing = 1;
                else if (t->udp_game_state < 0)
                    out->playing = 1;
                else
                    out->playing = 0;
                return 0;
            }
            cur_rd = RD_NONE;
            memset(out, 0, sizeof(*out));
            break;
        default: cur_rd = RD_NONE; break;
        }
        if (ok == 0 && out->connected)
            return 0;
        cur_rd = RD_NONE;               /* sticky source died: rediscover */
    }

    udp_poll(t);

    if (memfd_read_pcars(t, out) == 0) {
        cur_rd = RD_PCARS_MEMFD;
        return 0;
    }
    if (memfd_read_acevo(t, out) == 0) {
        cur_rd = RD_ACEVO;
        return 0;
    }
    if (sc_pcars1_read(out) == 0) {
        cur_rd = RD_PCARS1;
        return 0;
    }
    /* Extra sources (RaceRoom / AMS1 / rFactor 2) — additive, see
     * sc_telem_games.c. They fail fast when their game is not running. */
    if (sc_r3e_read(out) == 0) {
        cur_rd = RD_R3E;
        return 0;
    }
    if (sc_rf1_read(out) == 0) {
        cur_rd = RD_RF1;
        return 0;
    }
    if (sc_rf2_read(out) == 0) {
        cur_rd = RD_RF2;
        return 0;
    }

    if (!t->map)
        telem_try_attach(t, cfg);

    if (t->map) {
        struct pcars2APIStruct s;
        memset(&s, 0, sizeof(s));
        if (copy_consistent(t, &s) >= 0 && s.mVersion != 0) {
            if (fill_from_pcars(&s, out, SC_SRC_AMS2) == 0) {
                cur_rd = RD_LEGACY_MAP;
                return 0;
            }
        }
    }

    if (t->udp_have && (telem_now() - t->udp_last_t) < 1.5) {
        *out = t->udp_snap;
        if (t->udp_game_state == GAME_INGAME_PLAYING ||
            t->udp_game_state == GAME_INGAME_INMENU_TIME_TICKING ||
            t->udp_game_state == GAME_INGAME_RESTARTING)
            out->playing = 1;
        else if (t->udp_game_state < 0)
            out->playing = 1; /* physics stream is enough for the bicycle model */
        else
            out->playing = 0;
        cur_rd = RD_UDP;
        return 0;
    }
    cur_rd = RD_NONE;
    return 0;
}

void sc_telem_to_vehicle(const ScTelem *t, const ScConfig *cfg, ScVehicle *v) {
    memset(v, 0, sizeof(*v));
    if (!t->connected) return;

    float vx = t->local_vx;
    float vz = t->local_vz;
    float yaw = t->ang_y;
    if (cfg->swap_xz) {
        float tmp = vx;
        vx = t->local_vz;
        vz = tmp;
    }
    v->local_vx = vx * cfg->lat_sign;
    /* AC family (Evo, Rally) uses +Z forward. Pre-apply fwd_sign so
     * steer.c's vz * fwd_sign still yields forward (AMS2 keeps -1). */
    if (t->src == SC_SRC_ACEVO || t->src == SC_SRC_ACRALLY)
        v->local_vz = vz * ((cfg->fwd_sign < 0.f) ? -1.f : 1.f);
    else
        v->local_vz = vz;
    v->yaw_rate = yaw * cfg->yaw_sign;
    v->speed = t->speed;
    v->front_grounded = (float)t->front_grounded;
    v->valid = 1;
}

/* Wrapper: when every source fails this tick (torn sample rejected by a
 * sanity gate, frozen watchdog, scan in progress...), reuse the last good
 * sample for up to 150 ms instead of emitting zeroes. Zeroed telem forces
 * passthrough for that tick — the assist output jumps to raw stick and
 * back, which felt like the wheel slamming to one side. */
int sc_telem_read(ScTelemSrc *t, const ScConfig *cfg, ScTelem *out) {
    static ScTelem keep;
    static double keep_t = 0;
    static int have = 0;
    int r = sc_telem_read_sources(t, cfg, out);
    if (out->connected) {
        keep = *out;
        keep_t = telem_now();
        have = 1;
        return r;
    }
    if (have && (telem_now() - keep_t) < 0.15) {
        *out = keep;
        return 0;
    }
    have = 0;
    return r;
}
