/*
 * Runs inside the AMS2 Proton prefix AFTER the game has started.
 * Opens the game's Win32 mapping $pcars2$ (read-only) and copies it
 * into the Linux /dev/shm/$pcars2$ that simcontrol already mmaps.
 *
 * Unlike pcars2bridge.exe (CreateFileMapping / HELPERPROCESSFIRST) this
 * never owns the name, so it should not freeze Proton.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAP_NAME "$pcars2$"
#define UNIX_PATH "/dev/shm/$pcars2$"
#define MAP_SIZE 102288

int main(void)
{
    HANDLE h = NULL;
    printf("pcars2copy: waiting for Win32 mapping %s ...\n", MAP_NAME);
    fflush(stdout);
    while ((h = OpenFileMappingA(FILE_MAP_READ, FALSE, MAP_NAME)) == NULL) {
        Sleep(500);
    }
    void *src = MapViewOfFile(h, FILE_MAP_READ, 0, 0, MAP_SIZE);
    if (!src) {
        printf("pcars2copy: MapViewOfFile failed (%lu)\n", GetLastError());
        return 1;
    }

    int fd = open(UNIX_PATH, O_RDWR);
    if (fd < 0) {
        printf("pcars2copy: open %s failed\n", UNIX_PATH);
        return 1;
    }
    if (ftruncate(fd, MAP_SIZE) != 0) {
        /* size already set is fine */
    }
    void *dst = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (dst == MAP_FAILED) {
        printf("pcars2copy: mmap failed\n");
        return 1;
    }

    printf("pcars2copy: copying %s -> %s (Ctrl-C to stop)\n", MAP_NAME, UNIX_PATH);
    fflush(stdout);
    for (;;) {
        memcpy(dst, src, MAP_SIZE);
        Sleep(5);
    }
}
