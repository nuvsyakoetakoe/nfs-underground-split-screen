#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "local_camera.h"
#include "split_options.h"

/* Event layout used by Underground 1.4 between scanning and dispatch. */
typedef struct {
    BYTE event, value, port, binding;
    void *callback, *context;
} JoyEvent;
typedef struct { int count; JoyEvent events[256]; } JoyQueue;
_Static_assert(sizeof(JoyEvent) == 12, "32-bit joy queue");

enum { CAMERA = 0x75, LOOK_BACK = 0x76, STEER = 0x87, GAS = 0x89, BRAKE = 0x8c, HANDBRAKE = 0x91,
       SHIFT_UP = 0x93, SHIFT_DOWN = 0x94, RESET = 0x99, HORN = 0x9a, NOS = 0x9b,
       LANE_LEFT = 0xa6, LANE_RIGHT = 0xa7, LANE_RIGHT_ANALOG = 0xa9 };
static const BYTE actions[] = {STEER, GAS, BRAKE, HANDBRAKE, SHIFT_UP, SHIFT_DOWN, RESET, NOS,
                               CAMERA, LOOK_BACK, HORN, LANE_LEFT, LANE_RIGHT};
enum { ACTION_COUNT = sizeof(actions) };
typedef struct { BYTE value[ACTION_COUNT]; } Controls;
static BYTE previous[2][ACTION_COUNT];
static void *last_context[2][ACTION_COUNT];
static int ports[2] = {-1, -1};
static int assigned;

static Controls keyboard(int seat, int port) {
    int left = split_key(seat, port, 'A') || split_key(seat, port, VK_LEFT), right = split_key(seat, port, 'D') || split_key(seat, port, VK_RIGHT);
    Controls c = {{(BYTE)(128 + (right - left) * 127),
        split_key(seat, port, 'W') | split_key(seat, port, VK_UP), split_key(seat, port, 'S') | split_key(seat, port, VK_DOWN), split_key(seat, port, VK_SPACE),
        split_key(seat, port, VK_LSHIFT), split_key(seat, port, VK_LCONTROL), split_key(seat, port, 'R') | split_key(seat, port, VK_HOME), split_key(seat, port, VK_LMENU) | split_key(seat, port, 'N'),
        split_key(seat, port, 'C') | split_key(seat, port, VK_RETURN), split_key(seat, port, 'B') | split_key(seat, port, VK_RCONTROL),
        split_key(seat, port, 'H'), left && !right ? 255 : 0, right && !left ? 255 : 0}};
    return c;
}

static Controls gamepad(const XINPUT_GAMEPAD *p) {
    int x = p->sThumbLX;
    int magnitude = abs(x) - XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
    int delta = magnitude > 0 ? magnitude * 127 / (32767 - XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) : 0;
    WORD b = p->wButtons;
    int left = delta > 0 && x < 0, right = delta > 0 && x > 0;
    left |= !!(b & XINPUT_GAMEPAD_DPAD_LEFT);
    right |= !!(b & XINPUT_GAMEPAD_DPAD_RIGHT);
    Controls c = {{(BYTE)(128 + (x < 0 ? -delta : delta)),
        p->bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? p->bRightTrigger : 0,
        p->bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? p->bLeftTrigger : 0,
        b & XINPUT_GAMEPAD_A ? 255 : 0, b & XINPUT_GAMEPAD_RIGHT_SHOULDER ? 255 : 0,
        b & XINPUT_GAMEPAD_LEFT_SHOULDER ? 255 : 0, b & XINPUT_GAMEPAD_X ? 255 : 0,
        b & XINPUT_GAMEPAD_B ? 255 : 0, b & XINPUT_GAMEPAD_Y ? 255 : 0,
        b & XINPUT_GAMEPAD_RIGHT_THUMB ? 255 : 0, b & XINPUT_GAMEPAD_LEFT_THUMB ? 255 : 0, left && !right ? 255 : 0, right && !left ? 255 : 0}};
    return c;
}

static int owns_event(BYTE e) {
    return e == CAMERA || e == LOOK_BACK || (e >= STEER && e <= 0x96) || e == RESET || e == HORN || e == NOS || e == NOS + 1 ||
           (e >= LANE_LEFT && e <= LANE_RIGHT_ANALOG);
}

static void assign_devices(void) {
    split_assign_devices(ports);
    assigned = 1;
}

/* At this point the engine has scanned both ports and hasn't run callbacks.
   Resolve driving and camera commands through each driver's
   live registrations, even when DirectInput hasn't detected a second stick. */
static int route_input(void) {
    JoyQueue *q = (JoyQueue *)0x73b578;
    const int *race = *(int **)0x7361f0;
    if (*(int *)0x77a920 != 6 || *(int *)0x78a320 != 2 || !race || *race != 4) {
        camera_end_input();
        if (assigned && *(int *)0x77a920 != 6) {
            assigned = 0;
            ports[0] = ports[1] = -1;
        }
        memset(last_context, 0, sizeof last_context);
        return q->count;
    }
    if (!assigned) assign_devices();
    int kept = 0;
    for (int i = 0; i < q->count; ++i)
        if (!owns_event(q->events[i].event)) q->events[kept++] = q->events[i];
    q->count = kept;
    HWND focused = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(focused, &pid);
    camera_begin_input(pid == GetCurrentProcessId(), ports[0] < 0 ? 0 : ports[1] < 0 ? 1 : -1);
    for (int p = 0; p < 2; ++p) {
        Controls c = {{128}};
        XINPUT_STATE s = {0};
        if (pid == GetCurrentProcessId()) {
            if (ports[p] == PORT_KEYBOARD || ports[p] == PORT_RAW_KEYBOARD) c = keyboard(p, ports[p]);
            else {
                if (ports[p] >= 0 && XInputGetState(ports[p], &s) == ERROR_SUCCESS) c = gamepad(&s.Gamepad);
            }
        }
        camera_stick(p, s.Gamepad.sThumbRX, s.Gamepad.sThumbRY);
        for (unsigned i = 0; i < ACTION_COUNT; ++i) {
            unsigned a = i;
            /* Both Drag directions share one native latch. Release the old
               direction before pressing the new one on a direct reversal. */
            if (i >= ACTION_COUNT - 2 && c.value[ACTION_COUNT - 2])
                a = i == ACTION_COUNT - 2 ? ACTION_COUNT - 1 : ACTION_COUNT - 2;
            int before = q->count;
            typedef void (*QueueEvent)(unsigned, JoyQueue *);
            ((QueueEvent)0x573c30)((p << 16) | (c.value[a] << 8) | actions[a], q);
            if (before == q->count) { last_context[p][a] = NULL; continue; }
            JoyEvent *event = &q->events[before];
            /* The stock POV callback cycles even on a zero-valued event. */
            if ((actions[a] == CAMERA && !c.value[a]) ||
                (a >= 4 && previous[p][a] == c.value[a] && last_context[p][a] == event->context))
                q->count = before;
            previous[p][a] = c.value[a];
            last_context[p][a] = event->context;
        }
    }
    return q->count;
}

int install_local_input(void) {
    BYTE *site = (BYTE *)0x573fb4;
    if (memcmp(site, "\xa1\x78\xb5\x73\x00", 5)) return 0;
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    int32_t relative = (uintptr_t)route_input - (uintptr_t)site - 5;
    site[0] = 0xe8;
    memcpy(site+1, &relative, sizeof relative);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    VirtualProtect(site, 5, old, &old);
    return 1;
}
