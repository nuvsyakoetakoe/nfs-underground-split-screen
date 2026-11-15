#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "split_options.h"

/* Addresses refer only to the Underground 1.4 PC image used by this project. */
#define GAME_INT(va) (*(volatile int *)(uintptr_t)(va))
int install_world_views(void);
int install_local_input(void);
int install_local_menu(void);

/* PC track bundles contain the 2500-series scenery assets. The two-player
   streamer adds 50 to those IDs, requesting absent console variants. */
static int use_pc_scenery(void) {
    BYTE *branch = (BYTE *)0x444cd1;
    if (memcmp(branch-7, "\x83\x3d\x20\xa3\x78\x00\x02\x75\x3b", 9)) return 0;
    DWORD original;
    if (!VirtualProtect(branch, 1, PAGE_EXECUTE_READWRITE, &original)) return 0;
    *branch = 0xeb;
    FlushInstructionCache(GetCurrentProcess(), branch, 1);
    VirtualProtect(branch, 1, original, &original);
    return 1;
}

/* The race loader stores the two-player flag as its minimum vehicle asset
   LOD. Request the same geometry set used by a solo race for both seats. */
static int use_full_car_assets(void) {
    BYTE *set_minimum = (BYTE *)0x448fa7;
    if (memcmp(set_minimum - 3, "\x83\xfa\x02\x0f\x94\xc0\x33\xed", 8)) return 0;
    DWORD original;
    if (!VirtualProtect(set_minimum, 3, PAGE_EXECUTE_READWRITE, &original)) return 0;
    memcpy(set_minimum, "\x31\xc0\x90", 3);
    FlushInstructionCache(GetCurrentProcess(), set_minimum, 3);
    VirtualProtect(set_minimum, 3, original, &original);
    return 1;
}

static DWORD WINAPI record_race(void *unused) {
    (void)unused;
    FILE *log = fopen("SplitScreen-test.log", "w");
    if (!log) return 0;
    int previous_players = -1, previous_track = -1, previous_entries = -1;
    unsigned previous_state = ~0u;
    for (;;) {
        int players = GAME_INT(0x78a320);
        int track = GAME_INT(0x78a2f0);
        unsigned state = (unsigned)GAME_INT(0x77a920);
        int entries = GAME_INT(0x78a41c);
        if (players != previous_players || track != previous_track || state != previous_state ||
            entries != previous_entries) {
            fprintf(log, "time=%lu flow=%u local_slots=%d track=%d entries=%d ids=%d,%d\n",
                    GetTickCount(), state, players, track, entries,
                    GAME_INT(0x78a410), GAME_INT(0x78a414));
            fflush(log);
            previous_players = players;
            previous_track = track;
            previous_state = state;
            previous_entries = entries;
        }
        Sleep(50);
    }
}

__declspec(dllexport) void InitializeASI(void) {
    static LONG started;
    char enabled[2];
    int direct = GetEnvironmentVariableA("NFSU_SPLIT_TEST", enabled, sizeof enabled) == 1 &&
                 enabled[0] == '1';
    if (InterlockedExchange(&started, 1)) return;
    HMODULE game = GetModuleHandleW(NULL);
    if ((uintptr_t)game != 0x400000) return;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)game;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)((BYTE *)game + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x400721aa ||
        nt->OptionalHeader.SizeOfImage != 0x394368 ||
        memcmp((void *)0x4b6390, "\x55\x8b\xec\x83\xe4\xf0\x81\xec\x74\x0e\x00\x00", 12))
        return;
    /* The built-in SkipFE startup constructs a car entry for every local slot. */
    char solo[2];
    int players = GetEnvironmentVariableA("NFSU_SOLO_CONTROL", solo, sizeof solo) == 1 &&
                  solo[0] == '1' ? 1 : 2;
    if (!install_world_views()) return;
    if (players == 2 && (!use_pc_scenery() || !use_full_car_assets() ||
                         !install_local_input() || !install_split_race_names(direct))) return;
    if (!direct) {
        if (!install_local_menu()) return;
        return;
    }
    GAME_INT(0x70095c) = 1001;
    GAME_INT(0x6f1d48) = players;
    GAME_INT(0x734660) = 0;  /* AI starters; local starting positions follow them */
    GAME_INT(0x73465c) = 1;
    HANDLE logger = CreateThread(NULL, 0, record_race, NULL, 0, NULL);
    if (logger) CloseHandle(logger);
}
