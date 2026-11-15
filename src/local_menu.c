#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "local_garage.h"
#include "split_options.h"
#include <stdio.h>

static unsigned char selected_cars[2][0x720] __attribute__((aligned(16)));
static int selecting_player, first_selection, first_transmission;
static int session_active;
static unsigned char saved_unlock;
static int entry_selection;
static volatile int selections_ready;
typedef void *(WINAPI *CopyCar)(void *, const void *);
typedef uint32_t (__cdecl *TextureNameHash)(const char *, ...);

static void remember_car(int seat, const void *preview) {
    unsigned char *ride = selected_cars[seat];
    ((CopyCar)0x4323f0)(ride, preview);
    /* Both garage turns use preview slot 1. The race composites each
       driver's vinyl and wheel textures into the named slot for that seat. */
    TextureNameHash hash = (TextureNameHash)0x4f42f0;
    *(uint32_t *)(ride + 0x444) = hash("DUMMY_SKIN%d", seat + 1);
    *(uint32_t *)(ride + 0x448) = hash("DUMMY_WHEEL%d", seat + 1);
}

/* FEPrintf resolves labels through the live package or the FEng tree. */
__attribute__((used)) static const char heading_format[] = "%s";
__attribute__((naked)) static void set_heading(const char *package, const char *title) {
    __asm__ volatile(
        "pushl %edi\n movl 8(%esp),%eax\n movl $0x5fd1f82f,%edi\n"
        "pushl 12(%esp)\n pushl $_heading_format\n movl $0x4f6850,%ecx\n call *%ecx\n"
        "addl $8,%esp\n popl %edi\n ret\n");
}

__attribute__((used)) static void show_player(CarSelector *screen) {
    char text[112];
    garage_title(screen, selecting_player+1, text, sizeof text);
    char heading[144];
    const char *suffix = strstr(text," - ");
    snprintf(heading,sizeof heading,"%s%s",split_nickname(selecting_player),suffix ? suffix : "");
    set_heading("MU_QuickRaceCarSelect.fng", heading);
}

__attribute__((used)) static void start_session(void) {
    if (!session_active) {
        saved_unlock = *(unsigned char *)0x735ec1;
        entry_selection = *(int *)0x758c28;
    }
    *(unsigned char *)0x735ec1 = 1;
    session_active = 1;
    selecting_player = 0;
    selections_ready = 0;
}

__attribute__((used)) static void end_session(void) {
    if (session_active) {
        *(unsigned char *)0x735ec1 = saved_unlock;
        garage_leave();
        *(int *)0x758c28 = entry_selection;
        session_active = 0;
    }
}

__attribute__((naked)) static void main_open(void) {
    __asm__ volatile("pushal\n call _end_session\n popal\n pushl $0x4b30b0\n ret\n");
}

/* In local mode these three transitions normally open the simultaneous
   two-controller lobby. Use the PC garage for both turns instead. */
__attribute__((naked)) static void open_car_selector(void) {
    __asm__ volatile("movl $0x6c27e0,%edi\n pushl $0x4f6360\n ret\n");
}

/* The sequential selector previews one car in GarageMain for both turns. */
__attribute__((naked)) static void load_garage(void) {
    __asm__ volatile("movl $0x4f62f0,%eax\n call *%eax\n pushl $0x4bbf34\n ret\n");
}
__attribute__((naked)) static void notify_garage(void) {
    __asm__ volatile("pushl $0x746104\n pushl $0x4bbf7d\n ret\n");
}

__attribute__((naked)) static void garage_heading(void) {
    __asm__ volatile(
        "cmpl $4,0x777b4c\n jne 1f\n pushal\n pushl %ebp\n call _show_player\n addl $4,%esp\n popal\n ret\n"
        "1: pushl $0x495f00\n ret\n");
}

__attribute__((used)) static int confirm_car(CarSelector *screen) {
    if (!screen->selected) return 0;
    if (!garage_confirm(screen)) {
        set_heading("MU_QuickRaceCarSelect.fng", "COULD NOT SAVE BUILD - TRY AGAIN");
        return 0;
    }
    CarRow *selected = screen->selected;
    remember_car(selecting_player, selected->ride);
    if (!selecting_player) {
        first_selection = selected->key;
        first_transmission = *(int *)0x758954;
        selecting_player = 1;
        show_player(screen);
        return 0;
    }
    *(int *)0x758958 = *(int *)0x758954;
    *(int *)0x758954 = first_transmission;
    *(int *)0x758c28 = first_selection;
    selections_ready = 1;
    selecting_player = 0;
    return 1;
}

/* Back during the second turn returns control to the first driver. */
__attribute__((used)) static int previous_turn(CarSelector *screen) {
    garage_discard(screen);
    if (!selecting_player) return 0;
    selecting_player = 0;
    selections_ready = 0;
    *(int *)0x758954 = first_transmission;
    garage_focus(screen,first_selection);
    show_player(screen);
    return 1;
}

__attribute__((naked)) static void car_back(void) {
    __asm__ volatile(
        "cmpl $4,0x777b4c\n jne 1f\n pushal\n pushl %ebp\n call _previous_turn\n addl $4,%esp\n"
        "test %eax,%eax\n popal\n jz 1f\n pushl $0x4bcfbd\n ret\n"
        "1: movl 0xc(%ebp),%eax\n pushl %eax\n movl $0x6c2a08,%edi\n"
        "pushl $0x4bcd93\n ret\n");
}

/* Both transmission replies converge here with the package in EAX and
   its null check in EFLAGS. Keep both intact for the stock transition. */
__attribute__((naked)) static void transmission_confirmed(void) {
    __asm__ volatile(
        "pushfl\n cmpl $4,0x777b4c\n jne 1f\n pushal\n pushl %ebp\n"
        "call _confirm_car\n addl $4,%esp\n test %eax,%eax\n popal\n jnz 1f\n"
        "popfl\n pushl $0x4bcfbd\n ret\n"
        "1: popfl\n jz 2f\n movl $0xff,0x1c(%eax)\n"
        "2: pushl $0x4bcee7\n ret\n");
}

/* Drag uses the track transition directly, without a transmission popup. */
__attribute__((naked)) static void direct_track(void) {
    __asm__ volatile(
        "cmpl $4,0x777b4c\n jne 1f\n pushal\n pushl %ebp\n call _confirm_car\n"
        "addl $4,%esp\n test %eax,%eax\n popal\n jnz 1f\n"
        "pushl $0x4bcfbd\n ret\n"
        "1: movl 0xc(%ebp),%ecx\n pushl %ecx\n movl $0x6c256c,%edi\n"
        "pushl $0x4bccc5\n ret\n");
}

/* Native race constructors pass the local seat in EDX and destination on
   the stack. Copy the confirmed garage ride into the normal race entry. */
__attribute__((naked)) static void race_car(void) {
    __asm__ volatile(
        "cmpl $4,0x777b4c\n jne 1f\n cmpl $0,_selections_ready\n je 1f\n"
        "cmpl $1,%edx\n ja 1f\n imull $0x720,%edx,%eax\n addl $_selected_cars,%eax\n"
        "pushl %eax\n pushl 8(%esp)\n movl $0x4323f0,%eax\n call *%eax\n ret $4\n"
        "1: pushl $0x4ac0f0\n ret\n");
}

/* PC omits the Multiplayer case from the main menu's Accept switch.
   Rejoin the native Quick Race screen transition after setting the FE mode. */
__attribute__((naked)) static void accept_button(void) {
    __asm__ volatile(
        "cmpl $0x11793157,%eax\n jne 1f\n"
        "pushal\n call _start_session\n popal\n movl $4,0x777b4c\n pushl $0x4b34f3\n ret\n"
        "1: cmpl $0xa936c3a2,%eax\n pushl $0x4b33e0\n ret\n");
}

__attribute__((naked)) static void car_available(void) {
    __asm__ volatile(
        "cmpl $4,0x777b4c\n jne 1f\n movl $1,%eax\n ret\n"
        "1: pushl $0x5a1630\n ret\n");
}

/* The preset list checks each career reward directly, after testing its
   mode mask and resolving the reward record. Leave the profile untouched. */
__attribute__((naked)) static void preset_reward_available(void) {
    __asm__ volatile(
        "cmpl $0,_session_active\n je 1f\n cmpl $4,0x777b4c\n jne 1f\n"
        "testl %edx,%edx\n jz 1f\n movb $1,%cl\n ret\n"
        "1: movb 0x758ad8(%eax),%cl\n ret\n");
}

__attribute__((naked)) static void track_available(void) {
    __asm__ volatile(
        "cmpl $4,0x777b4c\n jne 1f\n movl $1,%eax\n ret $4\n"
        "1: pushl $0x5a16f0\n ret\n");
}

/* The PC car selector resets the FE participant count to one. The race
   constructor uses this count to populate both local drivers and their cars. */
__attribute__((naked)) static void car_select_players(void) {
    __asm__ volatile(
        "movl $1,0x748f70\n cmpl $4,0x777b4c\n jne 1f\n incl 0x748f70\n"
        "1: ret\n");
}

/* Extend the native selection list before the browser builds its icons. */
__attribute__((naked)) static void populate_list(void) {
    __asm__ volatile("movl 8(%ebp),%ebx\n cmpl $0,_session_active\n je 1f\n"
        "cmpl $4,0x777b4c\n jne 1f\n pushal\n pushl %ebx\n call _garage_populate\n"
        "addl $4,%esp\n popal\n 1: movl 0x40(%ebx),%esi\n ret\n");
}
__attribute__((naked)) static void editor_record(void) {
    __asm__ volatile("cmpl $0,_session_active\n je 1f\n cmpl $4,0x777b4c\n jne 1f\n"
        "pushl 24(%esp)\n call _garage_edit\n addl $4,%esp\n ret\n1: pushl $0x4ac060\n ret\n");
}

/* Three native predicates look up the editable model by its garage key. */
#define EDIT_KEY_HOOK(name, reg) \
__attribute__((naked)) static void name(void) { \
    __asm__ volatile("cmpl $0,_session_active\n je 1f\n cmpl $4,0x777b4c\n jne 1f\n" \
        "pushal\n pushl " reg "\n call _garage_base_key\n addl $4,%esp\n" \
        "movl %eax,24(%esp)\n popal\n ret\n 1: movl 0x738(" reg "),%ecx\n ret\n"); \
}
EDIT_KEY_HOOK(scroll_key,"%edx")
EDIT_KEY_HOOK(button_key,"%eax")
EDIT_KEY_HOOK(edit_key,"%ecx")

__attribute__((naked)) static void scroll_title(void) {
    __asm__ volatile("cmpl $0,_session_active\n je 1f\n cmpl $4,0x777b4c\n jne 1f\n"
        "pushal\n pushl %ebx\n call _garage_scrolled\n call _show_player\n addl $4,%esp\n popal\n"
        "1: pushl $0x746104\n pushl $0x4bd2e1\n ret\n");
}
/* Native Load Custom Car normally writes the loaded vehicle into the profile.
   In a split-screen edit, load it into the same draft as the parts editor. */
__attribute__((naked)) static void load_manual(void) {
    __asm__ volatile("cmpl $0,_session_active\n je 1f\n pushl %edi\n call _garage_load_manual\n"
        "addl $4,%esp\n test %eax,%eax\n jnz 2f\n"
        "1: movl $0x4abe90,%eax\n call *%eax\n movl $0x4ac060,%eax\n call *%eax\n"
        "2: pushl $0x4e7f7a\n ret\n");
}
/* Each race type has a different number of stock option rows. Zero marks
   events without an opponent field. Tournament already fields four cars. */
static const unsigned rival_row[] = {3, 2, 3, 0, 1, 2, 0};
static int rival_count[] = {0, 0, 0, 0, 2, 0, 0};

__attribute__((naked)) static void race_option_text(const char *package, const char *object,
                                                    const char *text) {
    __asm__ volatile("movl 4(%esp),%eax\n movl 8(%esp),%edx\n pushl 12(%esp)\n"
        "pushl $_heading_format\n movl $0x4f67f0,%ecx\n call *%ecx\n addl $8,%esp\n ret\n");
}

static void show_rival_count(BYTE *screen, unsigned mode) {
    char object[32];
    const char *package = *(const char **)(screen+0xc);
    snprintf(object,sizeof object,"Option%uData",rival_row[mode]);
    const char *values[] = {"0","1","2"};
    race_option_text(package,object,values[rival_count[mode]]);
}

static void __attribute__((thiscall)) change_rivals(BYTE *screen, unsigned message) {
    unsigned mode = *(unsigned *)0x777cc8;
    if (mode >= sizeof rival_row/sizeof rival_row[0] || !rival_row[mode]) return;
    int delta = message == 0x9120409e ? 2 : message == 0xb5971bf1 ? 1 : 0;
    rival_count[mode] = (rival_count[mode]+delta)%3;
    show_rival_count(screen,mode);
}

__attribute__((used)) static void add_rival_option(BYTE *screen) {
    unsigned mode = *(unsigned *)0x777cc8;
    if (*(int*)0x777b4c != 4 || mode >= sizeof rival_row/sizeof rival_row[0]) return;
    unsigned row = rival_row[mode];
    if (!row) return;
    /* Activate the same arrows, button and text group as a native row. */
    __asm__ volatile("pushl %1\n movl $0x4b8d50,%%eax\n call *%%eax"
        : : "b"(screen),"r"(row) : "eax","ecx","edx","cc","memory");
    *(void (**)(void))(screen+0x60+4*(row-1)) = (void (*)(void))change_rivals;
    char object[32];
    snprintf(object,sizeof object,"Option%uTextString",row);
    race_option_text(*(const char **)(screen+0xc),object,"AI OPPONENTS");
    show_rival_count(screen,mode);
}

__attribute__((naked)) static void race_options_ready(void) {
    __asm__ volatile("movl 0x60(%esp),%eax\n pushal\n pushl %eax\n call _add_rival_option\n"
        "addl $4,%esp\n popal\n addl $0x50,%eax\n pushl $0x4b8a2e\n ret\n");
}

__attribute__((used)) static int race_rivals(int stock) {
    unsigned mode = *(unsigned *)0x777cc8;
    if (*(int*)0x777b4c == 4 && *(int*)0x78a320 == 2 &&
        mode < sizeof rival_row/sizeof rival_row[0] && rival_row[mode]) return rival_count[mode];
    return stock;
}

/* The regular two-seat constructors write zero, while the tournament
   constructor passes its computed count in ECX. */
#define RIVAL_COUNT_HOOK(name, original) \
__attribute__((naked)) static void name(void) { \
    __asm__ volatile("pushfl\n pushal\n pushl " original "\n call _race_rivals\n" \
        "addl $4,%esp\n movl %eax,0x78a324\n popal\n popfl\n ret\n"); \
}
RIVAL_COUNT_HOOK(rivals_from_zero,"$0")
RIVAL_COUNT_HOOK(rivals_from_ecx,"%ecx")

/* The tournament schedule is already complete at this point. Only the
   opponent choice belongs on its final confirmation page. */
__attribute__((naked)) static void tournament_options(void) {
    __asm__ volatile("cmpl $4,0x777b4c\n je 1f\n pushl $0x4b9070\n ret\n 1: ret\n");
}

/* Tournament commits its track list and starts directly. Once the list
   is committed, offer the same pre-race options as the other events. */
__attribute__((naked)) static void tournament_start(void) {
    __asm__ volatile("cmpl $4,0x777b4c\n jne 1f\n cmpl $4,0x777cc8\n jne 1f\n"
        "movl 0xc(%ebp),%eax\n pushl $0xff\n movl $0x6c2588,%edi\n"
        "movl $0x4f63e0,%ecx\n call *%ecx\n addl $4,%esp\n ret\n"
        "1: pushl $0x4b5c00\n ret\n");
}

/* The native two-seat shuffle can swap overlapping pairs and leave two
   entries on one grid spot. Keep its valid positions, then fill the gaps.
   Finalize texture slots here too: preset and randomly generated AI cars
   take different paths through the native ride builder. */
__attribute__((used)) static void finalize_rival_grid(void) {
    int ai = *(int*)0x78a324;
    if (*(int*)0x777b4c != 4 || *(int*)0x78a320 != 2 || ai < 1 || ai > 2) return;
    BYTE *entries = *(BYTE **)0x78a418;
    int total = 2+ai;
    if (!entries || *(int*)0x78a41c < total) return;
    unsigned used = 0;
    TextureNameHash hash = (TextureNameHash)0x4f42f0;
    for (int i=0; i<total; ++i) {
        BYTE *entry = entries+i*0x750;
        unsigned spot = entry[3];
        if (spot && spot <= (unsigned)total && !(used & (1u<<spot))) used |= 1u<<spot;
        else entry[3] = 0;
        if (*(int*)(entry+4) == 2) {
            BYTE *ride = entry+0x10;
            *(uint32_t*)(ride+0x444) = hash("DUMMY_SKIN%d",i+1);
            *(uint32_t*)(ride+0x448) = hash("DUMMY_WHEEL%d",i+1);
        }
    }
    unsigned spot = 1;
    for (int i=0; i<total; ++i) {
        BYTE *entry = entries+i*0x750;
        if (entry[3]) continue;
        while (used & (1u<<spot)) ++spot;
        entry[3] = spot++;
    }
}
__attribute__((naked)) static void race_grid_ready(void) {
    __asm__ volatile("pushal\n call _finalize_rival_grid\n popal\n movl 0x777cc8,%edi\n ret\n");
}

int install_local_menu(void) {
    struct Site {
        BYTE *address;
        const char *expected;
        unsigned size;
        BYTE opcode;
        void (*hook)(void);
    } sites[] = {
        {(BYTE *)0x4b8a0c, "\xe8\x5f\x06\x00\x00", 5, 0xe8, tournament_options},
        {(BYTE *)0x4bb3f7, "\xe8\x04\xa8\xff\xff", 5, 0xe8, tournament_start},
        {(BYTE *)0x4b8a27, "\x8b\x44\x24\x60\x83\xc0\x50", 7, 0xe9, race_options_ready},
        {(BYTE *)0x4b4a1b, "\xc7\x05\x24\xa3\x78\x00\x00\x00\x00\x00", 10, 0xe8, rivals_from_zero},
        {(BYTE *)0x4b4c6b, "\xc7\x05\x24\xa3\x78\x00\x00\x00\x00\x00", 10, 0xe8, rivals_from_zero},
        {(BYTE *)0x4b4e9b, "\xc7\x05\x24\xa3\x78\x00\x00\x00\x00\x00", 10, 0xe8, rivals_from_zero},
        {(BYTE *)0x4b5600, "\x89\x0d\x24\xa3\x78\x00", 6, 0xe8, rivals_from_ecx},
        {(BYTE *)0x4b5334, "\x89\x0d\x24\xa3\x78\x00", 6, 0xe8, rivals_from_ecx},
        {(BYTE *)0x4b5c61, "\x8b\x3d\xc8\x7c\x77\x00", 6, 0xe8, race_grid_ready},
        {(BYTE *)0x4e7f70, "\xe8\x1b\x3f\xfc\xff\xe8\xe6\x40\xfc\xff", 10, 0xe9, load_manual},
        {(BYTE *)0x4bc5ad, "\x8b\x5d\x08\x8b\x73\x40", 6, 0xe8, populate_list},
        {(BYTE *)0x4bd0ce, "\xe8\x8d\xef\xfe\xff", 5, 0xe8, editor_record},
        {(BYTE *)0x4bd12c, "\x8b\x8a\x38\x07\x00\x00", 6, 0xe8, scroll_key},
        {(BYTE *)0x4bcb03, "\x8b\x88\x38\x07\x00\x00", 6, 0xe8, button_key},
        {(BYTE *)0x4bcfd3, "\x8b\x89\x38\x07\x00\x00", 6, 0xe8, edit_key},
        {(BYTE *)0x4bd2dc, "\x68\x04\x61\x74\x00", 5, 0xe9, scroll_title},
        {(BYTE *)0x4b33db, "\x3d\xa2\xc3\x36\xa9", 5, 0xe9, accept_button},
        {(BYTE *)0x4bc24b, "\xe8\xe0\x53\x0e\x00", 5, 0xe8, car_available},
        {(BYTE *)0x4bc34f, "\x8a\x88\xd8\x8a\x75\x00", 6, 0xe8, preset_reward_available},
        {(BYTE *)0x4b9f8e, "\xe8\x5d\x77\x0e\x00", 5, 0xe8, track_available},
        {(BYTE *)0x4bbf36, "\xc7\x05\x70\x8f\x74\x00\x01\x00\x00\x00", 10, 0xe8, car_select_players},
        {(BYTE *)0x4b2fe4, "\xe8\xc7\x00\x00\x00", 5, 0xe8, main_open},
        {(BYTE *)0x4b7bab, "\xe8\xb0\xe7\x03\x00", 5, 0xe8, open_car_selector},
        {(BYTE *)0x4bb287, "\xe8\xd4\xb0\x03\x00", 5, 0xe8, open_car_selector},
        {(BYTE *)0x4bb452, "\xe8\x09\xaf\x03\x00", 5, 0xe8, open_car_selector},
        {(BYTE *)0x4bbf27, "\x39\x3d\x4c\x7b\x77\x00\x74\x05", 8, 0xe9, load_garage},
        {(BYTE *)0x4bbf45, "\x39\x3d\x4c\x7b\x77\x00\x68\x04\x61\x74\x00\x75\x2b", 13, 0xe9, notify_garage},
        {(BYTE *)0x4bc049, "\xe8\xb2\x9e\xfd\xff", 5, 0xe8, garage_heading},
        {(BYTE *)0x4bcd8a, "\x8b\x45\x0c\x50\xbf\x08\x2a\x6c\x00", 9, 0xe9, car_back},
        {(BYTE *)0x4bcede, "\x74\x07\xc7\x40\x1c\xff\x00\x00\x00", 9, 0xe9, transmission_confirmed},
        {(BYTE *)0x4bccbc, "\x8b\x4d\x0c\x51\xbf\x6c\x25\x6c\x00", 9, 0xe9, direct_track},
        {(BYTE *)0x4b4b9c, "\xe8\x4f\x75\xff\xff", 5, 0xe8, race_car},
        {(BYTE *)0x4b4dda, "\xe8\x11\x73\xff\xff", 5, 0xe8, race_car},
        {(BYTE *)0x4b500a, "\xe8\xe1\x70\xff\xff", 5, 0xe8, race_car},
        {(BYTE *)0x4b5201, "\xe8\xea\x6e\xff\xff", 5, 0xe8, race_car},
        {(BYTE *)0x4b54da, "\xe8\x11\x6c\xff\xff", 5, 0xe8, race_car},
        {(BYTE *)0x4b590a, "\xe8\xe1\x67\xff\xff", 5, 0xe8, race_car},
        {(BYTE *)0x4b5b71, "\xe8\x7a\x65\xff\xff", 5, 0xe8, race_car}
    };
    for (unsigned i = 0; i < sizeof sites / sizeof sites[0]; ++i)
        if (memcmp(sites[i].address, sites[i].expected, sites[i].size)) return 0;
    for (unsigned i = 0; i < sizeof sites / sizeof sites[0]; ++i) {
        struct Site *s = &sites[i];
        DWORD old;
        if (!VirtualProtect(s->address, s->size, PAGE_EXECUTE_READWRITE, &old)) return 0;
        int32_t offset = (uintptr_t)s->hook - (uintptr_t)s->address - 5;
        s->address[0] = s->opcode;
        memcpy(s->address+1, &offset, 4);
        memset(s->address+5, 0x90, s->size-5);
        FlushInstructionCache(GetCurrentProcess(), s->address, s->size);
        VirtualProtect(s->address, s->size, old, &old);
    }
    return install_split_options();
}
