/* PE32+ helper: copy AMS2's Win32 $pcars2$ mapping into Linux /dev/shm/$pcars2$.
 * Run via proton AFTER the game is on track, with Shared Memory = Project Cars 2.
 * Does not CreateFileMapping — only OpenFileMapping — so it should not freeze Proton.
 */
#include <windows.h>
#include <stdio.h>

#define MAP_NAME "$pcars2$"
#define MAP_SIZE 102288

int main(void)
{
    HANDLE hMap = NULL;
    HANDLE hOut = INVALID_HANDLE_VALUE;
    void *src;
    DWORD wr;
    const char *paths[] = {
        "Z:\\dev\\shm\\$pcars2$",
        "\\\\unix\\dev\\shm\\$pcars2$",
        NULL
    };
    int i;

    printf("pcars2copy: waiting for Win32 mapping %s (enable Shared Memory = PC2)\n", MAP_NAME);
    fflush(stdout);
    while ((hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, MAP_NAME)) == NULL)
        Sleep(400);

    src = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, MAP_SIZE);
    if (!src) {
        printf("pcars2copy: MapViewOfFile failed (%lu)\n", (unsigned long)GetLastError());
        return 1;
    }

    for (i = 0; paths[i]; i++) {
        hOut = CreateFileA(paths[i], GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hOut != INVALID_HANDLE_VALUE) {
            printf("pcars2copy: writing %s\n", paths[i]);
            break;
        }
        printf("pcars2copy: cannot open %s (%lu)\n", paths[i], (unsigned long)GetLastError());
    }
    if (hOut == INVALID_HANDLE_VALUE)
        return 1;

    printf("pcars2copy: live copy, Ctrl-C / close this window to stop\n");
    fflush(stdout);
    for (;;) {
        if (SetFilePointer(hOut, 0, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
            break;
        if (!WriteFile(hOut, src, MAP_SIZE, &wr, NULL))
            break;
        Sleep(5);
    }
    return 0;
}
