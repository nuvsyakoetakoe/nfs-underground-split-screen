#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "local_camera.h"

typedef struct {
    float yaw, pitch, stick_x, stick_y;
    LARGE_INTEGER last_update;
    float idle;
    void *owner;
    int mode;
} Orbit;
static Orbit orbit[2];
static int mouse_seat;
static int active, mouse_enabled, mouse_x, mouse_y;
static float frequency, mouse_speed, stick_speed, timeout, return_speed;
static int enabled, invert, horizontal;

/* Observe the relative DirectInput sample before the game clamps its menu cursor. */
__attribute__((used)) static void mouse_sample(const int *mouse) {
    if (mouse_enabled) {
        mouse_x += mouse[3];
        mouse_y += mouse[4];
    }
}

__attribute__((naked)) static void observe_mouse(void) {
    __asm__ volatile("pushal; pushl %ebx; call _mouse_sample; addl $4,%esp; popal;"
                     "pushl $0x4131a0; ret");
}

static float stick_axis(short v) {
    const int dead = XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
    int length = abs(v);
    if (length <= dead) return 0;
    return copysignf((length-dead) / (float)((v < 0 ? 32768 : 32767)-dead), v);
}

void camera_stick(int seat, short x, short y) {
    orbit[seat].stick_x = stick_axis(x);
    orbit[seat].stick_y = horizontal ? 0 : stick_axis(y);
}

/* Called at the chase camera's final eye/target calculation. Returning zero
   hands the original call (including Widescreen Fix) its untouched registers. */
__attribute__((used)) static int orbit_camera(void *mover, const float *target, float *eye) {
    if (!active) return 0;
    for (int seat = 0; seat < 2; ++seat) {
        char *player = ((char **)0x7361bc)[seat];
        if (!player || *(void **)(player + 0x27c) != mover) continue;
        Orbit *o = &orbit[seat];
        int mode = *(int *)((char *)mover + 0x40);
        int mx = 0, my = 0;
        if (seat == mouse_seat && mouse_enabled) {
            mx = mouse_x; my = mouse_y;
            mouse_x = mouse_y = 0;
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = o->last_update.QuadPart ? fminf(0.05f, (now.QuadPart-o->last_update.QuadPart)/frequency) : 0;
        o->last_update = now;
        if (o->owner != mover || o->mode != mode || *((BYTE *)mover + 0x45)) {
            o->owner = mover; o->mode = mode;
            o->yaw = o->pitch = o->idle = 0;
        }
        if (!enabled || (mode != 0 && mode != 1) || *((BYTE *)mover + 0x45)) break;

        float dx = -mouse_speed * mx - stick_speed * o->stick_x * dt;
        float dy = mouse_speed * my - stick_speed * o->stick_y * dt;
        if (dx || dy) {
            o->yaw = remainderf(o->yaw + dx, 6.2831853f);
            o->pitch = fminf(1.0472f, fmaxf(-0.3491f, o->pitch + (invert ? -dy : dy)));
            o->idle = 0;
        } else {
            o->idle += dt;
            char *car = *(char **)(player + 4);
            if (o->idle >= timeout && car && fabsf(*(float *)(car + 0x3d0)) > 0.5f) {
                float decay = expf(-return_speed * dt);
                o->yaw *= decay; o->pitch *= decay;
                if (fabsf(o->yaw) + fabsf(o->pitch) < 0.001f) o->yaw = o->pitch = 0;
            }
        }
        if (o->yaw || o->pitch) {
            float x = eye[0]-target[0], y = eye[1]-target[1], z = eye[2]-target[2];
            float planar = hypotf(x,y), radius = hypotf(planar,z);
            float yaw = atan2f(y,x) + o->yaw;
            float pitch = fmaxf(-1.3963f, fminf(1.3963f, atan2f(z,planar) + o->pitch));
            eye[0] = target[0] + radius*cosf(pitch)*cosf(yaw);
            eye[1] = target[1] + radius*cosf(pitch)*sinf(yaw);
            eye[2] = target[2] + radius*sinf(pitch);
        }
        break;
    }
    return 1;
}

__attribute__((naked)) static void per_view_look_at(void) {
    __asm__ volatile("leal 0xd4(%esp),%esi; pushal; pushl %edi; pushl %eax; pushl %ebx;"
                     "call _orbit_camera; addl $12,%esp; testl %eax,%eax; popal; jz 1f;"
                     "pushl $0x48c171; pushl $0x40b780; ret;"
                     "1: pushl $0x48c16c; ret");
}

static int replace_branch(BYTE *site, const BYTE *expected, size_t size, BYTE opcode, void (*fn)(void)) {
    DWORD old;
    if (memcmp(site, expected, size) || !VirtualProtect(site,size,PAGE_EXECUTE_READWRITE,&old)) return 0;
    int32_t relative = (uintptr_t)fn - (uintptr_t)site - 5;
    site[0] = opcode;
    memcpy(site+1,&relative,4);
    memset(site+5,0x90,size-5);
    FlushInstructionCache(GetCurrentProcess(),site,size);
    VirtualProtect(site,size,old,&old);
    return 1;
}

static float setting(const char *ini, const char *key, const char *fallback) {
    char value[64];
    GetPrivateProfileStringA("CAMERA",key,fallback,value,sizeof value,ini);
    return strtof(value,NULL);
}

static int install_camera(void) {
    char ini[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL,ini,sizeof ini);
    if (!len || len >= sizeof ini) return 0;
    char *name = strrchr(ini,'\\');
    const char suffix[] = "scripts\\NFSUnderground.WidescreenFix.ini";
    if (!name || (size_t)(name+1-ini) + sizeof suffix > sizeof ini) return 0;
    memcpy(name+1,suffix,sizeof suffix);
    enabled = setting(ini,"Enable","1") != 0;
    invert = setting(ini,"InvertLook","0") != 0;
    horizontal = setting(ini,"OnlyAllowHorizontalStickMovement","0") != 0;
    mouse_speed = setting(ini,"MouseLookSensitivity","1") * 0.002618f;
    stick_speed = setting(ini,"StickLookSensitivity","1") * 1.0472f;
    timeout = setting(ini,"CameraReturnTimeout","3");
    return_speed = setting(ini,"CameraReturnSpeed","2");
    LARGE_INTEGER ticks;
    QueryPerformanceFrequency(&ticks);
    frequency = (float)ticks.QuadPart;
    /* Keep Widescreen Fix's own patches at 413130 and 48c16c available for solo. */
    if (memcmp((void *)0x48c171,"\x8b\x43\x10\x50",4)) return 0;
    if (!replace_branch((BYTE *)0x413187,(const BYTE *)"\xe8\x14\x00\x00\x00",5,0xe8,observe_mouse)) return 0;
    return replace_branch((BYTE *)0x48c165,(const BYTE *)"\x8d\xb4\x24\xd4\x00\x00\x00",7,0xe9,per_view_look_at);
}

void camera_begin_input(int focused, int keyboard_seat) {
    static int installed = -1;
    if (installed < 0) installed = install_camera();
    active = installed;
    mouse_seat = keyboard_seat;
    mouse_enabled = active && enabled && focused && keyboard_seat >= 0;
    if (!mouse_enabled) mouse_x = mouse_y = 0;
}

void camera_end_input(void) {
    if (!active) return;
    active = mouse_enabled = mouse_x = mouse_y = 0;
    memset(orbit,0,sizeof orbit);
}
