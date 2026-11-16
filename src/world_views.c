#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* Engine addresses refer to the checked 32-bit PC build. */
#define INT_AT(a) (*(int *)(uintptr_t)(a))
static int bottom_target[7];
static int top_target[7];
static int full_height;
static float blur_strength[2];
static int split;
static int full_effects, frame_presented;
static IDirect3DSurface9 *composited;
static int saved_target;
static int saved_bottom;
static int previous_split;
static BYTE saved_enabled;
static int logged[2];
static int render_diagnostics;
static volatile int lower_reflection;

static DWORD entered;
static unsigned captured;

/* Capture the game's own render target once after streaming has settled. */
static void capture_scene(void) {
    if (INT_AT(0x77a920) != 6 || entered == (DWORD)-1) return;
    int *race = *(int **)0x7361f0;
    if (!race || *race != 4) { entered = 0; return; }
    if (!entered) entered = GetTickCount();
    static const DWORD times[] = {5000, 35000, 50000};
    if (GetTickCount() - entered < times[captured]) return;
    HMODULE lib = LoadLibraryExW(L"d3dx9_43.dll", NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) { entered = (DWORD)-1; return; }
    typedef HRESULT (WINAPI *SaveSurface)(LPCWSTR, int, IDirect3DSurface9*, const PALETTEENTRY*, const RECT*);
    SaveSurface save = (SaveSurface)GetProcAddress(lib, "D3DXSaveSurfaceToFileW");
    IDirect3DDevice9 *device = *(IDirect3DDevice9 **)0x73636c;
    IDirect3DSurface9 *surface;
    if (save && SUCCEEDED(IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &surface))) {
        const wchar_t *names[] = {L"circuit-pair.png", L"circuit-pair-35s.png", L"circuit-pair-50s.png"};
        save(INT_AT(0x78a320)==2 ? names[captured] : L"circuit-solo.png", 3, surface, NULL, NULL);
        IDirect3DSurface9_Release(surface);
    }
    FreeLibrary(lib);
    if (++captured == 3 || INT_AT(0x78a320) == 1) entered = (DWORD)-1;
}

static HRESULT WINAPI end_frame(IDirect3DDevice9 *device) {
    HRESULT result = IDirect3DDevice9_EndScene(device);
    if (SUCCEEDED(result)) capture_scene();
    return result;
}

/* Widescreen Fix locates the original EndScene byte pattern at startup. */
static void enable_frame_capture(void) {
    static int attempted;
    if (attempted || INT_AT(0x77a920) != 6) return;
    attempted = 1;
    if (memcmp((void *)0x40a6cf, "\xff\x91\xa8\x00\x00\x00", 6)) return;
    DWORD original;
    if (!VirtualProtect((void *)0x40a6cf, 6, PAGE_EXECUTE_READWRITE, &original)) return;
    BYTE *end_scene = (BYTE *)0x40a6cf;
    int32_t end_distance = (uintptr_t)end_frame - (uintptr_t)end_scene - 5;
    end_scene[0] = 0xe8;
    memcpy(end_scene + 1, &end_distance, 4);
    end_scene[5] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), end_scene, 6);
    VirtualProtect(end_scene, 6, original, &original);
}

/* The director controls one camera. At the grid, offset its shot to P2.
   At the results screen both views follow the car chosen by the director. */
static float cinematic_camera[0x290 / sizeof(float)] __attribute__((aligned(16)));
static int native_lower_camera;

static void prepare_cinematic_camera(void) {
    int *race = *(int **)0x7361f0;
    const void *director = *(void **)0x740500;
    if (!race || !director) return;
    int intro = *race == 3;
    /* State 1 is the transition; race[5] is its destination. */
    int results = (*race == 8 || (*race == 1 && race[5] == 8)) && INT_AT(0x735ed0);
    if (!intro && !results) return;
    memcpy(cinematic_camera, director, sizeof cinematic_camera);
    int p1 = INT_AT(0x7361bc), p2 = INT_AT(0x7361c0);
    if (intro && p1 && p2 && INT_AT(p1 + 4) && INT_AT(p2 + 4)) {
        const float *a = (const float *)(INT_AT(p1 + 4) + 0x50);
        const float *b = (const float *)(INT_AT(p2 + 4) + 0x50);
        for (int axis = 0; axis < 3; ++axis) {
            float delta = b[axis] - a[axis];
            cinematic_camera[16 + axis] += delta;
            for (int col = 0; col < 3; ++col)
                cinematic_camera[12 + col] -= delta * cinematic_camera[4 * axis + col];
        }
    }
    native_lower_camera = INT_AT(0x740560);
    INT_AT(0x740560) = (int)(uintptr_t)cinematic_camera;
}

static void prepare_views(void) {
    /* A restart goes from the disabled loading view directly to mode 3.
       The PC target initializer only implements modes 0, 1 and 2. Use
       its normal world setup before deriving the two local viewports. */
    int bootstrap = INT_AT(0x77a920) == 6 && INT_AT(0x78a320) == 2 &&
                    INT_AT(0x7364a0) == 3 && !INT_AT(0x7364a8) &&
                    !*(BYTE *)0x7404c8;
    if (bootstrap) INT_AT(0x7364a8) = 1;
    ((void (*)(void))0x408410)();
    if (bootstrap) {
        INT_AT(0x7364a8) = 0;
        INT_AT(0x7364a4) = 3;
    }
    int *target = *(int **)0x740518;
    split = INT_AT(0x77a920) == 6 && INT_AT(0x78a320) == 2 &&
            target && target[4] && target[5] > 0 && target[6] >= 2 &&
            INT_AT(0x7404c0) && INT_AT(0x740520) && INT_AT(0x740560);
    if (!split) return;
    saved_target = (int)(uintptr_t)target;
    saved_enabled = *(BYTE *)0x740528;
    full_height = target[6];
    memcpy(top_target, target, sizeof top_target);
    memcpy(bottom_target, target, sizeof bottom_target);
    top_target[6] = full_height / 2;
    bottom_target[6] = full_height - top_target[6];
    bottom_target[1] = 2;  /* view index used by the matrix lookup */
    INT_AT(0x740518) = (int)(uintptr_t)top_target;
    INT_AT(0x740578) = (int)(uintptr_t)bottom_target;
    *(BYTE *)0x740528 = 1;
    prepare_cinematic_camera();
}

static void restore_views(void) {
    if (!split) return;
    if (native_lower_camera) {
        INT_AT(0x740560) = native_lower_camera;
        native_lower_camera = 0;
    }
    INT_AT(0x740518) = saved_target;
    INT_AT(0x740578) = saved_bottom;
    *(BYTE *)0x740528 = saved_enabled;
    split = 0;
}

static void begin_frame(void) {
    /* A lost D3D device can skip draw_world for the previous frame. */
    restore_views();
    saved_bottom = INT_AT(0x740578);
    prepare_views();
    if (split && !previous_split)
        blur_strength[0] = blur_strength[1] = 0.0f;
    previous_split = split;
    if (render_diagnostics) enable_frame_capture();
}

/* Apply the lens before the engine multiplies view and projection and builds
   its culling planes. Both players use their own moving camera and FOV. */
typedef float *(WINAPI *MatrixMultiply)(float *, const float *, const float *);
static float *WINAPI multiply_camera(float *out, const float *view, float *projection) {
    if (split) {
        for (int i = 0; i < 2; ++i) {
            uintptr_t camera_view = 0x7404c0 + i * 0x60;
            if (view != *(float **)camera_view) continue;
            uintptr_t camera = *(uintptr_t *)(camera_view + 0x40);
            unsigned fov = *(unsigned short *)(camera + 0xc4);
            if (fov == 0 || fov >= 32768) break;
            /* The game's reference lens is horizontal on a 4:3 display. */
            float vertical = (4.0f / 3.0f) / tanf(fov * (3.14159265f / 65536.0f));
            const int *target = i ? bottom_target : top_target;
            projection[0] = vertical * (float)target[6] / (float)target[5];
            projection[5] = -vertical;
            break;
        }
    }
    return ((MatrixMultiply)0x5ac69c)(out, view, projection);
}

/* The three graphics helpers restore a target's dimensions but reset its
   origin to (0,0). Keep the origin when restoring the lower player's view. */
static HRESULT WINAPI restore_viewport(IDirect3DDevice9 *device, const D3DVIEWPORT9 *vp) {
    if (split && !full_effects && *(int **)0x736458 == bottom_target) {
        D3DVIEWPORT9 lower = *vp;
        lower.Y = top_target[6];
        return IDirect3DDevice9_SetViewport(device, &lower);
    }
    return IDirect3DDevice9_SetViewport(device, vp);
}

/* The native projector uses a race-entry index, independent of view ID.
   Build P2 with the same car-pose and lens routines as the stock P1 light. */
static int second_light[24] __attribute__((aligned(16)));
static BYTE second_camera[0x2a0] __attribute__((aligned(16)));
static float second_matrices[0x120 / 4] __attribute__((aligned(16)));
static int dual_lights;

__attribute__((naked)) static void build_light(int *view) {
    __asm__ volatile("pushl %esi\n movl 8(%esp),%ecx\n pushl %ecx\n movl $1,%eax\n"
                     "movl $0x40a140,%edx\n call *%edx\n popl %ecx\n movl (%ecx),%esi\n"
                     "pushl $0\n pushl $0\n pushl $0\n pushl %ecx\n movl $0x40dd00,%eax\n"
                     "call *%eax\n popl %esi\n ret\n");
}

static void prepare_lights(void) {
    int *native = (int *)0x740760;
    dual_lights = split && INT_AT(0x700e80) && INT_AT(0x7361bc) &&
                  INT_AT(0x7361c0) && INT_AT(0x7361f0) &&
                  *(int *)(uintptr_t)INT_AT(0x7361f0) != 1 && native[16];
    if (!dual_lights) return;
    memcpy(second_light, native, sizeof second_light);
    memcpy(second_camera, (void *)(uintptr_t)native[16], 0x290);
    second_light[16] = (int)(uintptr_t)second_camera;
    second_light[0] = (int)(uintptr_t)second_matrices;
    build_light(second_light);
}

/* The game uses D3DX9's original effect ABI. SetMatrix is slot 0x98
   in the shipped runtime. Pass names as D3DXHANDLE string handles. */
__attribute__((used)) static void upload_light(float *world) {
    int *state = *(int **)0x73640c;
    if (!state || !state[6] || state[3]) return;
    void *effect = (void *)(uintptr_t)state[6];
    void **methods = *(void ***)effect;
    typedef HRESULT (WINAPI *SetMatrix)(void *, const char *, const float *);
    float projected[16] = {0}, tmp[16];
    if (dual_lights) {
        const float bias[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, .5f,.5f,0,1};
        MatrixMultiply mul = (MatrixMultiply)0x5ac69c;
        mul(tmp, world, second_matrices + 32);
        mul(projected, tmp, bias);
    }
    ((SetMatrix)methods[0x98 / 4])(effect, "SecondHeadlightWVP", projected);
}

__attribute__((naked)) static void headlight_shader(void) {
    __asm__ volatile("pushal\n pushl %ecx\n call _upload_light\n addl $4,%esp\n popal\n"
                     "pushl $0x4100a0\n ret\n");
}

/* The stock list only covers P1's beam. In split mode tag the visible
   receivers and let the two projectors clip their own footprints. */
__attribute__((naked)) static void light_receivers(void) {
    __asm__ volatile("cmpl $0,_dual_lights\n je 1f\n movl %ebx,%edx\n imull $0x60,%edx\n"
                     "addl $0x740460,%edx\n 1: pushl $0x4468a0\n ret\n");
}

/* All six faces share a texture. Rebuild them around the lower car
   after P1 has consumed it. The engine sets up P1 again next frame. */
__attribute__((naked)) static void update_face(int *face) {
    __asm__ volatile("pushl %esi\n movl 8(%esp),%ecx\n movl (%ecx),%esi\n"
                     "pushl $0\n pushl $0\n pushl $0\n pushl %ecx\n movl $0x40dd00,%eax\n"
                     "call *%eax\n popl %esi\n ret\n");
}

static void lower_environment(void) {
    uintptr_t player = INT_AT(0x7361c0);
    if (!player || !INT_AT(0x700e74)) return;
    uintptr_t car = *(uintptr_t *)(player + 4);
    if (!car) return;
    const float *position = (const float *)(car + 0x50);
    const float *offset = (const float *)0x704ad0;
    float center[4] = {position[0]+offset[0], position[1]+offset[1], position[2]+offset[2], 1};
    int camera = INT_AT(0x740500);
    INT_AT(0x740500) = INT_AT(0x740560);
    typedef void (WINAPI *FaceCameras)(int **, const float *, const float *);
    int **faces = (int **)0x78bd50;
    ((FaceCameras)0x40ad60)(faces, (float *)(uintptr_t)(INT_AT(0x740560)+0x40), center);
    INT_AT(0x740500) = camera;
    for (int i=0; i<6; ++i)
        if (faces[i] && faces[i][0] && faces[i][16]) update_face(faces[i]);
}

/* Render each camera through the stock full-frame filters, then reduce
   its completed image into its half of the real backbuffer. */
__attribute__((naked)) static void finish_effects(IDirect3DSurface9 *surface) {
    __asm__ volatile("pushl %ebp\n movl %esp,%ebp\n subl $16,%esp\n"
                     "movl 0x736460,%eax\n movl %eax,-16(%ebp)\n"
                     "movl 0x71ab9c,%eax\n movl %eax,-12(%ebp)\n"
                     "movl $-1,-8(%ebp)\n movl $-1,-4(%ebp)\n"
                     "pushl $2\n pushl $2\n pushl $0\n leal -8(%ebp),%eax\n pushl %eax\n"
                     "leal -16(%ebp),%eax\n pushl %eax\n pushl $1\n movl 8(%ebp),%eax\n"
                     "movl $0x40d6e0,%ecx\n call *%ecx\n leave\n ret\n");
}

__attribute__((naked)) static void final_composite(void) {
    __asm__ volatile("cmpl $0,_frame_presented\n je 1f\n ret\n 1: pushl $0x40d6e0\n ret\n");
}

/* The trail filter accumulates the preceding frame. Each seat needs
   its own history, even though the scene and blur scratch can be reused. */
static IDirect3DTexture9 *lower_trails;
static IDirect3DSurface9 *lower_trail_surface;
static void release_effects(void) {
    if (composited) IDirect3DSurface9_Release(composited);
    if (lower_trail_surface) IDirect3DSurface9_Release(lower_trail_surface);
    if (lower_trails) IDirect3DTexture9_Release(lower_trails);
    composited = NULL;
    lower_trail_surface = NULL;
    lower_trails = NULL;
    ((void (*)(void))0x40cce0)();
}

static int allocate_effects(IDirect3DDevice9 *device) {

    D3DSURFACE_DESC d;
    IDirect3DSurface9 *back = *(IDirect3DSurface9 **)0x736374;
    IDirect3DSurface9 *trail = *(IDirect3DSurface9 **)0x71abb0;
    if (!composited && SUCCEEDED(IDirect3DSurface9_GetDesc(back,&d)))
        IDirect3DDevice9_CreateRenderTarget(device,d.Width,d.Height,d.Format,
                D3DMULTISAMPLE_NONE,0,FALSE,&composited,NULL);
    if (trail && !lower_trails && SUCCEEDED(IDirect3DSurface9_GetDesc(trail,&d)) &&
        SUCCEEDED(IDirect3DDevice9_CreateTexture(device,d.Width,d.Height,1,D3DUSAGE_RENDERTARGET,
                     d.Format,D3DPOOL_DEFAULT,&lower_trails,NULL))) {
        if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(lower_trails,0,&lower_trail_surface)))
            IDirect3DDevice9_ColorFill(device,lower_trail_surface,NULL,0);
    }
    return composited && (!trail || lower_trail_surface);
}

/* The PC radial blur uses one NOS switch, one fade and the primary camera.
   Feed the native filter the state for the viewport currently being drawn.
   The active timestamp belongs to the vehicle physics, including races
   where the engine skips the optional player camera flag at +0x270. */
static int burning_nitrous(const BYTE *player) {
    const BYTE *car = *(BYTE *const *)(player + 4);
    if (!car) return 0;
    int started = *(const int *)(car + 0x3fc);
    return started != 0 && started != INT32_MAX;
}
__attribute__((used)) static void draw_speed_blur(int *view) {
    void (*draw)(void) = (void (*)(void))0x40cd40;
    int *race = *(int **)0x7361f0;
    BYTE **players = (BYTE **)0x7361bc;
    int lower = view == (int *)0x740520;
    if (!split || !race || *race != 4 || (!lower && view != (int *)0x7404c0) ||
        !players[0] || !players[1] || !view[0x40 / 4]) {
        draw();
        return;
    }
    int target = INT_AT(0x73654c);
    float *fade = (float *)0x73641c;
    float previous = *fade;
    int camera = INT_AT(0x740500);
    int boost[2] = {burning_nitrous(players[0]), burning_nitrous(players[1])};
    if (boost[0] || boost[1] || players[0][0x270] || players[1][0x270])
        INT_AT(0x73654c) = boost[lower];
    *fade = blur_strength[lower];
    INT_AT(0x740500) = view[0x40 / 4];
    draw();
    blur_strength[lower] = *fade;
    *fade = previous;
    INT_AT(0x73654c) = target;
    INT_AT(0x740500) = camera;
}
__attribute__((naked)) static void blur_current_view(void) {
    __asm__ volatile("pushl %edi; call _draw_speed_blur; addl $4,%esp; ret");
}

static void copy_finished_view(int player) {
    IDirect3DDevice9 *device = *(IDirect3DDevice9 **)0x73636c;
    IDirect3DSurface9 *back = *(IDirect3DSurface9 **)0x736374;
    finish_effects(composited);
    RECT dest = {0,player ? full_height/2 : 0,top_target[5],player ? full_height : full_height/2};
    HRESULT hr = IDirect3DDevice9_StretchRect(device,composited,NULL,back,&dest,D3DTEXF_LINEAR);
    if (FAILED(hr)) frame_presented = 0;
    if (player && SUCCEEDED(hr)) frame_presented = 1;
}

/* The road shader projects UVs with the current player's projection. Draw its
   texture with that exact matrix, including the owner's culling planes. The
   game's road target is shared, so consume each map before overwriting it. */
static void draw_player_worlds(void) {
    int *reflection = (int *)0x7405e0;
    int *target = (int *)(uintptr_t)reflection[0x58 / 4];
    void (*render)(void) = (void (*)(void))0x409580;
    if (!split) {
        render();
        return;
    }

    int draw_road = INT_AT(0x700e90) && ((BYTE *)reflection)[8] && target && target[4];
    int matrices = reflection[0], lens = reflection[3], camera = reflection[0x40 / 4];
    BYTE *top_enabled = (BYTE *)0x7404c8, *lower_enabled = (BYTE *)0x740528;
    BYTE top = *top_enabled, lower = *lower_enabled;
    for (int player = 0; player < 2; ++player) {
        int *owner = (int *)(uintptr_t)(0x7404c0 + player * 0x60);
        if (!(player ? lower : top)) continue;
        if (draw_road) {
            lower_reflection = player;
            reflection[0] = owner[0];
            reflection[3] = owner[3];
            reflection[0x40 / 4] = owner[0x40 / 4];
        }
        *top_enabled = !player;
        *lower_enabled = player;
        /* Material lighting reads the environment quality flag during the
           main draw. Suppress duplicate mirror work without disabling paint. */
        BYTE *mirror = (BYTE *)0x740588;
        BYTE mirror_enabled = *mirror;
        if (player) {
            *mirror = 0;
            lower_environment();
        }
        int trail_texture = INT_AT(0x71aba4), trail_surface = INT_AT(0x71abb0);
        int old_camera = INT_AT(0x740500);
        /* The bloom and motion-trail passes reference the P1 view directly. */
        int *main_view = (int *)0x7404c0;
        int backup[24];
        if (player && full_effects) {
            if (trail_texture && trail_surface) {
                INT_AT(0x71aba4) = (int)(uintptr_t)lower_trails;
                INT_AT(0x71abb0) = (int)(uintptr_t)lower_trail_surface;
            }
            memcpy(backup, main_view, sizeof backup);
            memcpy(main_view, owner, sizeof backup);
            main_view[1] = 2;
            ((BYTE *)main_view)[8] = 0;
        }
        render();
        if (player && full_effects) memcpy(main_view, backup, sizeof backup);
        INT_AT(0x740500) = old_camera;
        INT_AT(0x71aba4) = trail_texture;
        INT_AT(0x71abb0) = trail_surface;
        if (full_effects) copy_finished_view(player);
        *mirror = mirror_enabled;
    }
    *top_enabled = top;
    *lower_enabled = lower;
    reflection[0] = matrices;
    reflection[3] = lens;
    reflection[0x40 / 4] = camera;
    lower_reflection = 0;
}

/* The native reflected-car selector uses view 4 for P1 and 5 for P2.
   Only its owner selection needs the alternate ID. The shared render target
   and the visibility bits used later in the same function still use view 4. */
__attribute__((naked)) static void reflected_car_owner(void) {
    __asm__ volatile(
        "movl 4(%esi),%eax\n movl 0x78a410,%edi\n"
        "cmpl $4,%eax\n jne 1f\n cmpl $0,_lower_reflection\n je 1f\n incl %eax\n"
        "1: ret\n");
}

static void draw_world(void) {
    /* Use the direct view path until the game has allocated its effect surfaces. */
    int effect = INT_AT(0x736384);
    int light_streaks = INT_AT(0x700e9c);
    int use_trail = light_streaks && INT_AT(0x700ea0) && INT_AT(0x73645c) == 1;
    frame_presented = full_effects = 0;
    IDirect3DDevice9 *device = *(IDirect3DDevice9 **)0x73636c;
    if (split && effect && INT_AT(0x73645c) == 1 && INT_AT(0x736460) &&
        INT_AT(0x71ab9c) && INT_AT(0x736374) && (!use_trail || INT_AT(0x71abb0))) {
        if (allocate_effects(device)) {
            full_effects = 1;
            top_target[6] = bottom_target[6] = full_height;
        }
    }
    if (split && !full_effects) {
        INT_AT(0x736384) = 0;
        /* The native glow pass binds the trail surface in this device mode. */
        if (use_trail) INT_AT(0x700e9c) = 0;
    }
    prepare_lights();
    draw_player_worlds();
    dual_lights = full_effects = 0;
    if (split) {
        INT_AT(0x736384) = effect;
        if (!frame_presented && use_trail) INT_AT(0x700e9c) = light_streaks;
    }
    if (!split) return;
    restore_views();
    if (frame_presented) {
        IDirect3DDevice9_SetRenderTarget(device,0,*(IDirect3DSurface9 **)0x736374);
    }
    D3DVIEWPORT9 full = {0, 0, top_target[5], full_height, 0, 1};
    IDirect3DDevice9_SetViewport(device, &full);
}

__attribute__((used)) static void select_half(int *target, int clear) {
    int bottom = target == bottom_target;
    IDirect3DDevice9 *device = *(IDirect3DDevice9 **)0x73636c;
    D3DVIEWPORT9 vp = {0, bottom && !full_effects ? top_target[6] : 0, target[5], target[6], 0, 1};
    HRESULT result = IDirect3DDevice9_SetViewport(device, &vp);
    if (SUCCEEDED(result) && clear)
        IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER |
                               D3DCLEAR_STENCIL, 0xff000000, 1, 0);
    if (render_diagnostics && !logged[bottom]) {
        D3DVIEWPORT9 actual;
        if (SUCCEEDED(IDirect3DDevice9_GetViewport(device, &actual))) {
            FILE *f = fopen("SplitScreen-views.log", bottom ? "a" : "w");
            if (f) {
                fprintf(f, "view=%d x=%lu y=%lu width=%lu height=%lu result=%08lx\n",
                        bottom + 1, actual.X, actual.Y, actual.Width, actual.Height, result);
                fprintf(f, "preset=%d worldLOD=%d carLOD=%d road=%d headlights=%d motionBlur=%d fullEffects=%d\n",
                        INT_AT(0x736334), INT_AT(0x700eb0), INT_AT(0x700eb4),
                        INT_AT(0x700e90), INT_AT(0x700e80), INT_AT(0x700e98),
                        full_effects);
                fprintf(f, "display=");
                for (int i = 0; i < 17; ++i)
                    fprintf(f, "%s%d", i ? "," : "", INT_AT(0x700e74 + 4*i));
                fputc('\n', f);
                if (INT_AT(0x700e90) && INT_AT(0x7405e0)) {
                    const float *m = *(float **)0x7405e0;
                    fprintf(f, "road camera=%08x projection=(%.5f,%.5f)\n",
                            (unsigned)INT_AT(0x740620), m[16], m[21]);
                }
                fclose(f);
            }
        }
        logged[bottom] = 1;
    }
}

/* Engine ABI: ESI carries the target, caller pops the clear flag. */
__attribute__((naked)) static void bind_world(void) {
    __asm__ volatile(
        "cmpl $0, _split\n je 1f\n"
        "pushl $0\n movl $0x40abd0,%eax\n call *%eax\n addl $4,%esp\n"
        "pushl 4(%esp)\n pushl %esi\n call _select_half\n addl $8,%esp\n ret\n"
        "1: movl $0x40abd0,%eax\n jmp *%eax\n");
}

static HRESULT WINAPI compile_effect(void *device, const char *src, UINT size,
        void *defines, void *includes, DWORD flags, void *pool, void **fx, void **errors) {
    typedef HRESULT (WINAPI *Compile)(void *, const char *, UINT, void *, void *, DWORD, void *, void **, void **);
    HRESULT hr = ((Compile)0x5afb2f)(device, src, size, defines, includes, flags, pool, fx, errors);
    if (FAILED(hr) && errors && *errors) {
        void *buffer = *errors;
        void **vtable = *(void ***)buffer;
        const char *message = ((const char *(WINAPI *)(void *))vtable[3])(buffer);
        FILE *f = fopen("SplitScreen-shaders.log", "a");
        if (f) { fprintf(f, "%s\n", message); fclose(f); }
    }
    return hr;
}

/* Record the mesh indices after all distance thresholds and native overrides.
   The render-info pointer is shared across views, so identify the car by seat. */
__attribute__((used)) static void trace_car_detail(int view, void *render_info, int body, int wheels) {
    static unsigned seen;
    if ((view != 1 && view != 2) || INT_AT(0x77a920) != 6) return;
    uintptr_t race = INT_AT(0x7361f0);
    if (!race || INT_AT(race) != 4) return;
    for (int seat = 0; seat < 2; ++seat) {
        uintptr_t player = INT_AT(0x7361bc + seat * 4);
        if (!player) continue;
        uintptr_t car = *(uintptr_t *)(player + 4);
        if (!car || *(void **)(car + 0x4d4) != render_info) continue;
        unsigned bit = 1u << ((view - 1) * 2 + seat);
        if (seen & bit) return;
        FILE *log = fopen("SplitScreen-car-detail.log", seen ? "a" : "w");
        if (log) {
            uintptr_t info = (uintptr_t)render_info;
            uintptr_t shell = (unsigned)body < 4 ? *(uintptr_t *)(info + 0x1058 + 24 * body) : 0;
            uintptr_t rim = (unsigned)wheels < 3 ? *(uintptr_t *)(info + 0x14d8 + 24 * wheels) : 0;
            fprintf(log, "view=%d car=P%d body=%d %.64s wheels=%d %.64s\n", view, seat + 1,
                    body, shell ? (char *)(shell + 0x94) : "(none)",
                    wheels, rim ? (char *)(rim + 0x94) : "(none)");
            fclose(log);
        }
        seen |= bit;
        return;
    }
}

__attribute__((naked)) static void trace_selected_mesh(void) {
    __asm__ volatile("pushal\n pushl %ecx\n pushl %esi\n pushl %edi\n"
                     "movl 12(%ebp),%eax\n pushl 4(%eax)\n call _trace_car_detail\n"
                     "addl $16,%esp\n popal\n movl 4(%edi),%eax\n testl %eax,%eax\n ret\n");
}

/* ESI is the view in both vehicle passes. The native mode-3 branches
   multiply projected size by 0.7 and the mesh pass also forces LOD >= 1.
   Return ZF clear for the two visible player views only. */
__attribute__((naked)) static void car_detail_mode(void) {
    __asm__ volatile("cmpl $0,_split\n je 1f\n cmpl $1,4(%esi)\n je 2f\n"
                     "cmpl $2,4(%esi)\n jne 1f\n 2: cmpl $0,4(%esi)\n ret\n"
                     "1: cmpl $3,0x7364a0\n ret\n");
}

/* EBX is the active view. The director's actor loop skips spectators when
   mode == 3. Keep the native flag for mirrors and other auxiliary passes. */
__attribute__((naked)) static void crowd_detail_mode(void) {
    __asm__ volatile("sete 0x1f(%esp)\n pushfl\n cmpl $0,_split\n je 1f\n"
                     "cmpl $1,4(%ebx)\n je 2f\n cmpl $2,4(%ebx)\n jne 1f\n"
                     "2: movb $0,0x23(%esp)\n 1: popfl\n ret\n");
}

static int install_race_detail(void) {
    struct Site { BYTE *addr; const char *original; size_t length; void (*target)(void); } sites[] = {
        {(BYTE *)0x56d972, "\x83\x3d\xa0\x64\x73\x00\x03", 7, car_detail_mode},
        {(BYTE *)0x56dc56, "\x83\x3d\xa0\x64\x73\x00\x03", 7, car_detail_mode},
        {(BYTE *)0x592126, "\x0f\x94\x44\x24\x1b", 5, crowd_detail_mode},
        {(BYTE *)0x56e15d, "\x8b\x47\x04\x85\xc0", 5, trace_selected_mesh},
    };
    unsigned count = render_diagnostics ? 4 : 3;
    for (unsigned i = 0; i < count; ++i)
        if (memcmp(sites[i].addr, sites[i].original, sites[i].length)) return 0;
    DWORD old[4];
    for (unsigned i = 0; i < count; ++i) {
        if (VirtualProtect(sites[i].addr, sites[i].length, PAGE_EXECUTE_READWRITE, &old[i])) continue;
        while (i > 0) {
            --i;
            DWORD ignored;
            VirtualProtect(sites[i].addr, sites[i].length, old[i], &ignored);
        }
        return 0;
    }
    for (unsigned i = 0; i < count; ++i) {
        BYTE *site = sites[i].addr;
        int32_t offset = (uintptr_t)sites[i].target - (uintptr_t)site - 5;
        site[0] = 0xe8;
        memcpy(site + 1, &offset, 4);
        memset(site + 5, 0x90, sites[i].length - 5);
    }
    for (unsigned i = count; i-- > 0;) {
        DWORD ignored;
        FlushInstructionCache(GetCurrentProcess(), sites[i].addr, sites[i].length);
        VirtualProtect(sites[i].addr, sites[i].length, old[i], &ignored);
    }
    return 1;
}

int install_world_views(void) {
    char diagnostics[2];
    render_diagnostics = GetEnvironmentVariableA("NFSU_RENDER_DIAGNOSTICS", diagnostics, sizeof diagnostics) == 1 &&
                         diagnostics[0] == '1';
    struct Edit { uintptr_t site; void (*to)(void); uintptr_t original; } calls[] = {
        {0x40a586, begin_frame, 0x408410}, {0x4098b4, bind_world, 0x40abd0},
        {0x40a671, draw_world, 0x409580}, {0x4099ba, blur_current_view, 0x40cd40},
        {0x40a6b6, final_composite, 0x40d6e0},
        {0x40a504, release_effects, 0x40cce0}, {0x40b4c7, release_effects, 0x40cce0},
        {0x4118e4, release_effects, 0x40cce0},
        {0x40e382, (void (*)(void))compile_effect, 0x5afb2f},
        {0x40ee63, headlight_shader, 0x4100a0}, {0x4098d3, light_receivers, 0x4468a0},
        {0x40df29, (void (*)(void))multiply_camera, 0x5ac69c}
    };
    if (memcmp((void *)0x409b5d, "\x83\xfb\x01\x0f\x8e", 5)) return 0;
    BYTE *car_owner = (BYTE *)0x570de6;
    if (memcmp(car_owner, "\x8b\x46\x04\x8b\x3d\x10\xa4\x78\x00", 9)) return 0;
    for (unsigned i = 0; i < sizeof calls / sizeof calls[0]; ++i) {
        uintptr_t site = calls[i].site;
        int32_t distance;
        memcpy(&distance, (void *)(site + 1), 4);
        if (*(BYTE *)site != 0xe8 || site + 5 + distance != calls[i].original) return 0;
    }
    const uintptr_t restores[] = {0x40d34e, 0x40d6c1, 0x40da08};
    for (unsigned i = 0; i < 3; ++i) {
        const BYTE *site = (BYTE *)restores[i];
        if (site[0] != 0xff || site[1] != (i == 2 ? 0x91 : 0x92) ||
            memcmp(site+2, "\xbc\x00\x00\x00", 4)) return 0;
    }

    if (!install_race_detail()) return 0;
    DWORD old;
    BYTE *page = (BYTE *)0x409000;
    if (!VirtualProtect(page, 0x9000, PAGE_EXECUTE_READWRITE, &old)) return 0;
    DWORD car_protection;
    if (!VirtualProtect(car_owner, 9, PAGE_EXECUTE_READWRITE, &car_protection)) {
        VirtualProtect(page, 0x9000, old, &old);
        return 0;
    }
    int32_t owner_distance = (uintptr_t)reflected_car_owner - (uintptr_t)car_owner - 5;
    car_owner[0] = 0xe8;
    memcpy(car_owner + 1, &owner_distance, 4);
    memset(car_owner + 5, 0x90, 4);
    FlushInstructionCache(GetCurrentProcess(), car_owner, 9);
    VirtualProtect(car_owner, 9, car_protection, &car_protection);
    for (unsigned i = 0; i < sizeof calls / sizeof calls[0]; ++i) {
        int32_t distance = (uintptr_t)calls[i].to - calls[i].site - 5;
        memcpy((void *)(calls[i].site + 1), &distance, 4);
    }
    for (unsigned i = 0; i < 3; ++i) {
        BYTE *site = (BYTE *)restores[i];
        int32_t distance = (uintptr_t)restore_viewport - restores[i] - 5;
        site[0] = 0xe8;
        memcpy(site + 1, &distance, 4);
        site[5] = 0x90;
    }
    *(BYTE *)0x409b5f = 2;
    FlushInstructionCache(GetCurrentProcess(), page, 0x9000);
    VirtualProtect(page, 0x9000, old, &old);
    return 1;
}
