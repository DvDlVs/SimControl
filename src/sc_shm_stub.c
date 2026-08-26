#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pcars2data.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;
static void on_sig(int s) { (void)s; running = 0; }

int main(void) {
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    const char *name = "/$pcars2$";
    size_t sz = (size_t)PCARS2_SIZE;
    if (sz < sizeof(struct pcars2APIStruct)) sz = sizeof(struct pcars2APIStruct);

    shm_unlink(name);
    int fd = shm_open(name, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        fprintf(stderr, "simcontrol-shm: shm_open %s: %s\n", name, strerror(errno));
        return 1;
    }
    if (ftruncate(fd, (off_t)sz) != 0) {
        fprintf(stderr, "simcontrol-shm: ftruncate: %s\n", strerror(errno));
        return 1;
    }
    void *m = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        fprintf(stderr, "simcontrol-shm: mmap: %s\n", strerror(errno));
        return 1;
    }
    memset(m, 0, sz);
    fprintf(stderr, "simcontrol-shm: created %s (%zu bytes). Leave this running.\n", name, sz);
    fprintf(stderr, "  file: /dev/shm/$pcars2$\n");
    fprintf(stderr, "  next: run pcars2bridge.exe in the AMS2 Proton prefix, then AMS2.\n");
    fprintf(stderr, "  Ctrl-C to stop.\n");

    while (running) pause();

    munmap(m, sz);
    close(fd);
    /* Do not unlink: the game/bridge may still hold it. */
    return 0;
}
