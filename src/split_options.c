#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <xinput.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>

/* Omitted by the bundled MinGW cfgmgr32.h. */
CMAPI CONFIGRET WINAPI CM_Get_Device_Interface_PropertyW(PCWSTR, const DEVPROPKEY *,
    DEVPROPTYPE *, PBYTE, PULONG, ULONG);
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "split_options.h"

/* The profile and race entry each reserve 16 bytes for a driver name. */
typedef struct {
    char names[2][SPLIT_NAME_BYTES];
    char devices[2][32];  /* Auto, Pad1..4 or a hash of a raw keyboard's Windows path. */
} Settings;
static Settings saved, draft;
static wchar_t ini[MAX_PATH];
static int loaded;
static HWND input_window;
static int raw_available;
static UINT forwarded_key;
static HWND raw_target;
static DWORD raw_thread;
static HHOOK raw_hook;
typedef struct { HANDLE handle; char id[32]; unsigned group; BYTE down[256]; } Keyboard;
static Keyboard keyboards[32];
static unsigned keyboard_count;
static const char *pad_ids[] = {"Pad1", "Pad2", "Pad3", "Pad4"};
static BYTE pads[4];
static int editing_seat = -1;
static char editing_name[SPLIT_NAME_BYTES];
static DWORD ignore_until;
static int page_dirty;
static int replace_name;
static int device_seat=-1;
static char identified[32];

static void load_settings(void) {
    if (loaded) return;
    loaded = 1;
    DWORD n = GetModuleFileNameW(NULL, ini, MAX_PATH);
    wchar_t *leaf = n && n < MAX_PATH ? wcsrchr(ini, L'\\') : NULL;
    if (leaf && MAX_PATH - (leaf-ini) > 30) wcscpy(leaf+1, L"SplitScreenOptions.ini");
    else ini[0] = 0;
    for (int p = 0; p < 2; ++p) {
        wchar_t section[16], name[32], device[32];
        swprintf(section, 16, L"Player%d", p+1);
        GetPrivateProfileStringW(section,L"Nickname",L"",name,32,ini);
        GetPrivateProfileStringW(section,L"Device",L"Auto",device,32,ini);
        if (!WideCharToMultiByte(CP_ACP,0,name,-1,saved.names[p],SPLIT_NAME_BYTES,NULL,NULL))
            saved.names[p][0] = 0;
        WideCharToMultiByte(CP_ACP,0,device,-1,saved.devices[p],32,NULL,NULL);
        if (!saved.devices[p][0]) strcpy(saved.devices[p],"Auto");
    }
}
const char *split_nickname(int seat) {
    load_settings();
    return saved.names[seat][0] ? saved.names[seat] : seat ? "Player 2" : "Player 1";
}
static int save_settings(void) {
    wchar_t temp[MAX_PATH];
    if (!ini[0] || swprintf(temp,MAX_PATH,L"%ls.tmp",ini) < 0) return 0;
    HANDLE f=CreateFileW(temp,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if (f==INVALID_HANDLE_VALUE) return 0;
    wchar_t text[300], names[2][32], devices[2][32];
    for (int p=0; p<2; ++p) {
        MultiByteToWideChar(CP_ACP,0,draft.names[p],-1,names[p],32);
        MultiByteToWideChar(CP_ACP,0,draft.devices[p],-1,devices[p],32);
    }
    int length=swprintf(text,300,L"\xfeff[Player1]\r\nNickname=%ls\r\nDevice=%ls\r\n"
        L"[Player2]\r\nNickname=%ls\r\nDevice=%ls\r\n",names[0],devices[0],names[1],devices[1]);
    DWORD wrote, size=(DWORD)length*sizeof(wchar_t);
    int ok=length>0 && WriteFile(f,text,size,&wrote,NULL) && wrote==size && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok || !MoveFileExW(temp,ini,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp);
        return 0;
    }
    saved=draft;
    return 1;
}

enum { MAX_PHYSICAL = 64 };
typedef struct {
    GUID container;
    char model[96];
    int key, primary_mouse, primary_key, gamepad, driving_key_seen;
} PhysicalInput;
static const GUID no_container={0};
static PhysicalInput physical[MAX_PHYSICAL];
static unsigned physical_count;
static RAWINPUTDEVICELIST *last_raw;
static UINT last_raw_count;
static int devices_changed = 1;

static uint64_t path_hash(const wchar_t *path) {
    uint64_t hash=14695981039346656037ULL;
    for (; *path; ++path) hash=(hash ^ (unsigned)*path)*1099511628211ULL;
    return hash;
}

static int container_of(DEVINST node, GUID *id) {
    static const GUID computer={0,0,0,{0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}};
    ULONG bytes=sizeof *id;
    DEVPROPTYPE type;
    return CM_Get_DevNode_PropertyW(node,&DEVPKEY_Device_ContainerId,&type,(BYTE*)id,&bytes,0)==CR_SUCCESS
        && type==DEVPROP_TYPE_GUID && !IsEqualGUID(id,&no_container) && !IsEqualGUID(id,&computer);
}

static void describe_interface(const wchar_t *path, GUID *container, char model[96]) {
    wchar_t instance[512], text[256]={0};
    DEVPROPTYPE type;
    ULONG size=sizeof instance;
    DEVINST node;
    if (CM_Get_Device_Interface_PropertyW(path,&DEVPKEY_Device_InstanceId,&type,(BYTE*)instance,&size,0)
        !=CR_SUCCESS || CM_Locate_DevNodeW(&node,instance,0)!=CR_SUCCESS) return;
    GUID id;
    if (container_of(node,&id)) *container=id;
    for (;;) {
        size=sizeof text;
        if (CM_Get_DevNode_PropertyW(node,&DEVPKEY_Device_BusReportedDeviceDesc,&type,
                                     (BYTE*)text,&size,0)==CR_SUCCESS && text[0]) break;
        size=sizeof text;
        if (CM_Get_DevNode_PropertyW(node,&DEVPKEY_Device_FriendlyName,&type,
                                     (BYTE*)text,&size,0)==CR_SUCCESS && text[0]) break;
        DEVINST parent;
        GUID parent_id;
        if (IsEqualGUID(container,&no_container) || CM_Get_Parent(&parent,node,0)!=CR_SUCCESS ||
            !container_of(parent,&parent_id) || !IsEqualGUID(container,&parent_id)) return;
        node=parent;
    }
    char encoded[1024];
    if (WideCharToMultiByte(CP_ACP,0,text,-1,encoded,sizeof encoded,NULL,NULL))
        snprintf(model,96,"%s",encoded);
}

static void enumerate_devices(void) {
    XINPUT_STATE state;
    for (int i=0; i<4; ++i) pads[i]=XInputGetState(i,&state)==ERROR_SUCCESS;
    UINT n=0;
    if (GetRawInputDeviceList(NULL,&n,sizeof(RAWINPUTDEVICELIST))==(UINT)-1) return;
    RAWINPUTDEVICELIST *list=calloc(n?n:1,sizeof(*list));
    if (!list) return;
    UINT got=GetRawInputDeviceList(list,&n,sizeof(*list));
    if (got==(UINT)-1 || (!devices_changed && got==last_raw_count &&
                          (!got || !memcmp(list,last_raw,got*sizeof(*list))))) {
        free(list);
        return;
    }
    Keyboard next[32]={0};
    unsigned used=0;
    physical_count=0;
    memset(physical,0,sizeof physical);
    for (UINT i=0;i<got;++i) {
        DWORD kind=list[i].dwType;
        wchar_t path[1024];
        UINT chars=1024;
        if (GetRawInputDeviceInfoW(list[i].hDevice,RIDI_DEVICENAME,path,&chars)==(UINT)-1) continue;
        if (kind==RIM_TYPEHID && !wcsstr(path,L"&IG_")) continue;
        GUID id={0};
        char model[96]={0};
        describe_interface(path,&id,model);
        unsigned g=0;
        while (g<physical_count && (IsEqualGUID(&id,&no_container) ||
               !IsEqualGUID(&id,&physical[g].container))) ++g;
        if (g==MAX_PHYSICAL) continue;
        PhysicalInput *device=&physical[g];
        if (g==physical_count) {
            device->container=id;
            device->key=-1;
            ++physical_count;
        }
        if (!device->model[0] && model[0]) strcpy(device->model,model);
        int primary=wcsstr(path,L"&MI_00")!=NULL;
        if (kind==RIM_TYPEMOUSE) {
            device->primary_mouse |= primary;
        } else if (kind==RIM_TYPEKEYBOARD && used<32) {
            Keyboard *key=&next[used];
            key->group=g;
            key->handle=list[i].hDevice;
            snprintf(key->id,sizeof key->id,"Key%016llx",(unsigned long long)path_hash(path));
            for (unsigned k=0;k<keyboard_count;++k)
                if (keyboards[k].handle==key->handle && !strcmp(keyboards[k].id,key->id))
                    memcpy(key->down,keyboards[k].down,sizeof key->down);
            if (device->key<0 || (primary && !device->primary_key) ||
                (primary==device->primary_key && strcmp(key->id,next[device->key].id)<0))
                device->key=(int)used;
            device->primary_key |= primary;
            ++used;
        } else if (kind==RIM_TYPEHID) device->gamepad=1;
    }
    memcpy(keyboards,next,sizeof next);
    keyboard_count=used;
    free(last_raw);
    last_raw=list;
    last_raw_count=got;
    devices_changed=0;
}
static void record_key(HANDLE device,UINT vk,USHORT flags) {
    if (vk>=255) return;
    unsigned i=0;
    while (i<keyboard_count && keyboards[i].handle!=device) ++i;
    if (i==keyboard_count) {
        /* DirectInput's registration may omit device-change notifications. */
        static DWORD last_refresh;
        DWORD now=GetTickCount();
        if (last_refresh && now-last_refresh<500) return;
        last_refresh=now;
        enumerate_devices();
        i=0;
        while (i<keyboard_count && keyboards[i].handle!=device) ++i;
    }
    if (i<keyboard_count) {
        keyboards[i].down[vk]=(flags&RI_KEY_BREAK)?0:255;
        if (device_seat>=0 && !(flags&RI_KEY_BREAK)) {
            PhysicalInput *device=&physical[keyboards[i].group];
            /* A combo receiver can expose a real keyboard on a secondary
               interface. Offer its keys when the user identifies it by typing. */
            if (device->primary_mouse && !device->primary_key && !device->driving_key_seen) {
                if (vk!='W' && vk!='A' && vk!='S' && vk!='D') return;
                device->driving_key_seen=1;
            }
            strcpy(identified,keyboards[i].id);
            page_dirty=1;
        }
    }
}
/* DirectInput can own keyboard registration on its private window. Observe
   that queue without changing its target, flags or delivery to the game. */
static void read_key(LPARAM param,int forward) {
    if (GetForegroundWindow()!=input_window ||
        (device_seat<0 && (*(int*)0x77a920!=6 || *(int*)0x78a320!=2))) return;
    RAWINPUT raw;
    UINT size=sizeof raw;
    if (GetRawInputData((HRAWINPUT)param,RID_INPUT,&raw,&size,sizeof(RAWINPUTHEADER))==(UINT)-1 ||
        raw.header.dwType!=RIM_TYPEKEYBOARD || !raw.header.hDevice) return;
    UINT vk=raw.data.keyboard.VKey;
    USHORT flags=raw.data.keyboard.Flags;
    if (vk==VK_SHIFT) vk=MapVirtualKeyW(raw.data.keyboard.MakeCode,MAPVK_VSC_TO_VK_EX);
    if (vk==VK_CONTROL) vk=(flags&RI_KEY_E0)?VK_RCONTROL:VK_LCONTROL;
    if (vk==VK_MENU) vk=(flags&RI_KEY_E0)?VK_RMENU:VK_LMENU;
    if (forward) PostMessageW(input_window,forwarded_key,(WPARAM)raw.header.hDevice,MAKELPARAM(vk,flags));
    else record_key(raw.header.hDevice,vk,flags);
}
static LRESULT CALLBACK observe_raw(int code,WPARAM removed,LPARAM value) {
    if (code>=0 && removed==PM_REMOVE) {
        const MSG *m=(const MSG*)value;
        if (m->hwnd==raw_target && m->message==WM_INPUT) read_key(m->lParam,1);
    }
    return CallNextHookEx(NULL,code,removed,value);
}
static int type_name(WPARAM key, LPARAM flags) {
    if (editing_seat<0) return 0;
    size_t len=strlen(editing_name);
    if (key==VK_ESCAPE || key==VK_RETURN) {
        if (key==VK_RETURN) strcpy(draft.names[editing_seat],editing_name);
        editing_seat=-1;
        ignore_until=GetTickCount()+250;
    } else if (key==VK_BACK) {
        if (replace_name) len=0;
        else if (len) len=CharPrevA(editing_name,editing_name+len)-editing_name;
        editing_name[len]=0;
        replace_name=0;
    } else {
        BYTE state[256]; wchar_t ch[4]; char encoded[8];
        if (!GetKeyboardState(state) || ToUnicode((UINT)key,(flags>>16)&255,state,ch,4,0)!=1 ||
            ch[0]<32 || ch[0]==127 || ch[0]==L'"' || ch[0]==L'\'') return 1;
        BOOL substituted=FALSE;
        UINT cp=GetACP();
        int n=WideCharToMultiByte(cp,cp==CP_UTF8?0:WC_NO_BEST_FIT_CHARS,ch,1,encoded,8,
                                 NULL,cp==CP_UTF8?NULL:&substituted);
        if (replace_name) len=0;
        if (n>0 && !substituted && n+len<SPLIT_NAME_BYTES) {
            memcpy(editing_name+len,encoded,n);
            editing_name[len+n]=0;
            replace_name=0;
        }
    }
    page_dirty=1;
    return 1;
}
static LRESULT CALLBACK game_messages(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    if (msg==forwarded_key && forwarded_key) {
        if (GetForegroundWindow()==w) record_key((HANDLE)wp,LOWORD(lp),HIWORD(lp));
        return 0;
    }
    if (msg==WM_INPUT && GET_RAWINPUT_CODE_WPARAM(wp)==RIM_INPUT) read_key(lp,0);
    if (msg==WM_INPUT_DEVICE_CHANGE) { devices_changed=1; enumerate_devices(); page_dirty=1; }
    if (msg==WM_KILLFOCUS)
        for (unsigned i=0;i<keyboard_count;++i) memset(keyboards[i].down,0,256);
    if (msg==WM_KEYDOWN && type_name(wp,lp)) return 0;
    if (editing_seat>=0 && (msg==WM_CHAR || msg==WM_KEYUP)) return 0;
    if (msg==WM_NCDESTROY) {
        RemoveWindowSubclass(w,game_messages,id);
        if (raw_hook) UnhookWindowsHookEx(raw_hook);
        raw_hook=NULL;
        raw_thread=0;
        input_window=NULL;
        raw_available=0;
    }
    return DefSubclassProc(w,msg,wp,lp);
}
static void attach_input(void) {
    HWND w=input_window;
    if (!w) {
        w=GetActiveWindow();
        if (!w || GetWindowThreadProcessId(w,NULL)!=GetCurrentThreadId()) return;
        if (!SetWindowSubclass(w,game_messages,1,0)) return;
        input_window=w;
        forwarded_key=RegisterWindowMessageW(L"NFSU.SplitScreen.RawKeyboard.1");
    }
    RAWINPUTDEVICE reg[32]; UINT n=32;
    UINT found=GetRegisteredRawInputDevices(reg,&n,sizeof *reg);
    if (found!=(UINT)-1) {
        for (UINT i=0;i<found;++i) if (reg[i].usUsagePage==1 && reg[i].usUsage==6) {
            HWND target=reg[i].hwndTarget;
            if (!target || target==w) {
                if (raw_hook) UnhookWindowsHookEx(raw_hook);
                raw_hook=NULL; raw_target=NULL; raw_thread=0;
                raw_available=1;
                return;
            }
            DWORD pid=0,thread=GetWindowThreadProcessId(target,&pid);
            if (pid!=GetCurrentProcessId()) {raw_available=0;return;}
            if (raw_hook && thread!=raw_thread) {UnhookWindowsHookEx(raw_hook);raw_hook=NULL;}
            raw_target=target;
            raw_thread=thread;
            if (!raw_hook) raw_hook=SetWindowsHookExW(WH_GETMESSAGE,observe_raw,NULL,thread);
            raw_available=raw_hook!=NULL;
            return;
        }
    }
    RAWINPUTDEVICE keyboard={1,6,RIDEV_DEVNOTIFY,w};
    raw_available=RegisterRawInputDevices(&keyboard,1,sizeof keyboard);
}
static int raw_slot(const char *id) {
    for (unsigned k=0;k<keyboard_count;++k) if (!strcmp(id,keyboards[k].id)) return (int)k;
    return -1;
}
/* Existing INI files identify individual keyboard collections. Resolve them
   to the same physical device as the row in the picker. */
static const PhysicalInput *input_for(const char *id) {
    int slot=raw_slot(id);
    return slot>=0 ? &physical[keyboards[slot].group] : NULL;
}
static int same_input(const char *a,const char *b) {
    if (!strcmp(a,b)) return 1;
    const PhysicalInput *device=input_for(a);
    return device && device==input_for(b);
}
BYTE split_key(int seat,int port,unsigned vk) {
    if (port==PORT_KEYBOARD) return GetAsyncKeyState(vk)&0x8000 ? 255:0;
    int slot=raw_slot(saved.devices[seat]);
    if (slot<0 || vk>=256) return 0;
    /* Preserve old assignments if a pair of separate keyboards on one
       receiver was explicitly bound by collection. */
    int other=raw_slot(saved.devices[1-seat]);
    if (other>=0 && keyboards[other].group==keyboards[slot].group) return keyboards[slot].down[vk];
    for (unsigned i=0;i<keyboard_count;++i)
        if (keyboards[i].group==keyboards[slot].group && keyboards[i].down[vk]) return 255;
    return 0;
}
static int fixed_port(const char *id) {
    for (int i=0;i<4;++i) if (!strcmp(id,pad_ids[i])) return i;
    if (!strcmp(id,"Keyboard")) return PORT_KEYBOARD;
    if (!strncmp(id,"Key",3)) return PORT_RAW_KEYBOARD;
    return PORT_UNAVAILABLE;
}
void split_assign_devices(int ports[2]) {
    load_settings();
    attach_input();
    enumerate_devices();
    char layout[16]={0};
    GetEnvironmentVariableA("NFSU_INPUT_LAYOUT",layout,sizeof layout);
    int count=pads[0]+pads[1]+pads[2]+pads[3];
    int force_pads=!strcmp(layout,"TwoPads");
    int force_keys=!strcmp(layout,"KeyboardPad");
    for (int p=0;p<2;++p) ports[p]=fixed_port(saved.devices[p]);
    for (int p=0;p<2;++p) {
        if (strcmp(saved.devices[p],"Auto")) continue;
        int other=ports[1-p];
        if (!p && !force_pads && (force_keys || count<2) &&
            other!=PORT_KEYBOARD && other!=PORT_RAW_KEYBOARD) {
            ports[p]=PORT_KEYBOARD;
            continue;
        }
        for (int i=0;i<4;++i) if (pads[i] && i!=other) {ports[p]=i;break;}
        if (ports[p]!=PORT_UNAVAILABLE) continue;
        if (other>=0 && !force_pads) ports[p]=PORT_KEYBOARD;
        else for (int i=0;i<4;++i) if (i!=other) {ports[p]=i;break;}
    }
}
/* Native selector state. The menu owns both the button array and its objects. */
enum { OPTIONS_BUTTON=0x4236a077, TITLE=0x5b9d88b9, DESCRIPTION=0x446044e3 };
static const uint32_t row_keys[]={0x0272d751,0x541307a8,0x9955,0x7f007e43,OPTIONS_BUTTON,
    0x368a0075,0x58c164c4,0x689e2e13,0xfd8a3b6d,0x8816789f};
static const char selector[]="MU_OptionsSelector_PC.fng";
static BYTE *page;
static void *page_owner;
static BYTE *original_buttons[16];
static DWORD original_flags[16],original_count;
static DWORD next_poll;
static const char *notice;
static uint32_t object_key(const BYTE *obj) { return *(const uint32_t*)(obj+0x10); }

static BYTE *find_package(const char *name) {
    BYTE *p;
    __asm__ volatile("movl $0x4f65d0,%%edx;call *%%edx":"=a"(p) : "0"(name):"edx","ecx","cc","memory");
    return p;
}
static BYTE *package(void) { return find_package(selector); }
static BYTE *object(BYTE *pkg,uint32_t hash) {
    BYTE *result;
    __asm__ volatile("movl $0x4ffb70,%%eax;call *%%eax":"=a"(result),"+c"(hash),"+d"(pkg)::"cc","memory");
    return result;
}
static void label(BYTE *pkg,uint32_t hash,const char *text) {
    BYTE *obj=object(pkg,hash);
    if (!obj || *(int*)(obj+0x18)!=2) return;
    ((void(WINAPI*)(void*,const char*))0x41f060)(obj,text);
    *(DWORD*)(obj+0x1c)|=2;
}
/* A focus notification for another row can arrive while Init is fading
   this one in. Only leave HighLight if this row actually held focus. */
static void update_row_focus(BYTE *pkg,uint32_t key,uint32_t group,uint32_t selected) {
    if (!pkg) return;
    BYTE *button=object(pkg,key);
    BYTE *bar=object(pkg,group);
    if (!button || !bar) return;
    BYTE *current=*(BYTE**)(button+0x54);
    int highlighted=current && *(uint32_t*)(current+0x30)==0x249db7b7;
    if (highlighted==(selected==key)) return;
    const char *script=selected==key?"HighLight":"UnHighLight";
    BYTE *objects[]={button,bar};
    for (unsigned i=0;i<2;++i)
        __asm__ volatile("pushl $0;pushl %1;movl $0x4f6b70,%%eax;call *%%eax;addl $8,%%esp"
            ::"D"(objects[i]),"r"(script):"eax","ecx","edx","cc","memory");
}
static void focus(uint32_t hash) {
    __asm__ volatile("pushl 0x73578c;movl $0x4f5ed0,%%edx;call *%%edx"
        :"+a"((const char*){selector}),"+c"(hash)::"edx","cc","memory");
}
static int row_number(uint32_t key) {
    for (int i=0;i<10;++i) if (key==row_keys[i]) return i;
    return -1;
}
static int offers_keyboard(const PhysicalInput *device) {
    if (device->key<0 || device->gamepad) return 0;
    if (!device->primary_mouse || device->primary_key || device->driving_key_seen) return 1;
    /* Keep explicit bindings to a keyboard on a shared receiver selectable. */
    return input_for(draft.devices[0])==device || input_for(draft.devices[1])==device;
}
static void device_label(const char *id,char *out,size_t size) {
    int p=fixed_port(id);
    if (p>=0) {
        const PhysicalInput *match=NULL;
        unsigned candidates=0;
        for (unsigned i=0;i<physical_count;++i) if (physical[i].gamepad) {
            match=&physical[i];
            ++candidates;
        }
        if (candidates==1 && pads[0]+pads[1]+pads[2]+pads[3]==1 && pads[p] && match->model[0])
            snprintf(out,size,"%s",match->model);
        else snprintf(out,size,"XInput Gamepad %d%s",p+1,pads[p]?"":" (disconnected)");
        return;
    }
    const PhysicalInput *device=input_for(id);
    if (device) {
        const char *model=device->model;
        unsigned twins=0;
        for (unsigned i=0;i<physical_count;++i)
            if (offers_keyboard(&physical[i]) && !strcmp(physical[i].model,model)) ++twins;
        if (twins>1) snprintf(out,size,"Keyboard %u  %s",(unsigned)(device-physical)+1,model);
        else snprintf(out,size,"Keyboard%s%s",*model?"  ":"",model);
    } else if (p==PORT_RAW_KEYBOARD) snprintf(out,size,"Keyboard (disconnected)");
    else snprintf(out,size,"%s",p==PORT_KEYBOARD?"Keyboard":"Auto");
}
static const char *choices[MAX_PHYSICAL+6];
static unsigned choice_count, first_choice, visible_choices;
static int more_devices;
static void set_visible(unsigned count) {
    BYTE **buttons=*(BYTE***)(page+0x84);
    for (unsigned i=0;i<original_count;++i) {
        BYTE *button=original_buttons[i];
        int row=row_number(object_key(button));
        if (row>=0 && (unsigned)row<count) {
            ((void(*)(void*))0x4f6970)(button);
            buttons[row]=button;
        } else ((void(*)(void*))0x4f6910)(button);
    }
    *(DWORD*)(page+0x88)=count;
}
static void device_choices(void) {
    choice_count=0;
    choices[choice_count++]="Auto";
    if (!raw_available || !keyboard_count) choices[choice_count++]="Keyboard";
    for (unsigned i=0;i<physical_count;++i) {
        PhysicalInput *device=&physical[i];
        if (raw_available && offers_keyboard(device)) choices[choice_count++]=keyboards[device->key].id;
    }
    for (int i=0;i<4;++i) if (pads[i]) choices[choice_count++]=pad_ids[i];
    if (first_choice>=choice_count) first_choice=0;
    more_devices=choice_count>10;
    visible_choices=choice_count-first_choice;
    if (more_devices && visible_choices>9) visible_choices=9;
}
static void draw_devices(void) {
    char text[96],name[48];
    snprintf(text,sizeof text,"PLAYER %d - INPUT DEVICE",device_seat+1);
    label(page,TITLE,text);
    for (unsigned i=0;i<visible_choices;++i) {
        const char *id=choices[first_choice+i];
        device_label(id,name,sizeof name);
        snprintf(text,sizeof text,"%s%s",name,same_input(id,draft.devices[device_seat])?"  [Selected]":
            strcmp(id,"Auto") && same_input(id,draft.devices[1-device_seat])?"  [Other player]":"");
        label(page,row_keys[i],text);
    }
    if (more_devices) label(page,row_keys[visible_choices],"MORE DEVICES");
    if (identified[0]) {
        device_label(identified,name,sizeof name);
        snprintf(text,sizeof text,"Input detected on %s",name);
        label(page,DESCRIPTION,text);
    } else label(page,DESCRIPTION,"Press a key or gamepad button to identify your device.");
}
static void enter_devices(int seat) {
    device_seat=seat;
    identified[0]=0;
    enumerate_devices();
    first_choice=0;
    device_choices();
    for (unsigned i=0;i<choice_count;++i)
        if (same_input(choices[i],draft.devices[seat]) && more_devices) first_choice=(i/9)*9;
    device_choices();
    set_visible(visible_choices+more_devices);
    unsigned row=0;
    for (unsigned i=0;i<visible_choices;++i)
        if (same_input(choices[first_choice+i],draft.devices[seat])) row=i;
    focus(row_keys[row]);
    draw_devices();
}
static void draw_page(void) {
    if (device_seat>=0) {draw_devices();page_dirty=0;return;}
    char text[80],device[40];
    label(page,TITLE,"SPLIT SCREEN OPTIONS");
    for (int p=0;p<2;++p) {
        const char *name=draft.names[p][0]?draft.names[p]:p?"Player 2":"Player 1";
        if (editing_seat==p) snprintf(text,sizeof text,"P%d NAME   %s_",p+1,editing_name);
        else snprintf(text,sizeof text,"P%d NAME   %s",p+1,name);
        label(page,row_keys[p*2],text);
        device_label(draft.devices[p],device,sizeof device);
        snprintf(text,sizeof text,"P%d DEVICE   %s",p+1,device);
        label(page,row_keys[p*2+1],text);
    }
    label(page,OPTIONS_BUTTON,"SAVE SETTINGS");
    label(page,DESCRIPTION,notice?notice:editing_seat>=0?"Type a nickname. Enter to confirm, Esc to cancel.":
          "Select a name or device. Back cancels changes.");
    page_dirty=0;
}
static void end_devices(void) {
    unsigned row=2*device_seat+1;
    device_seat=-1;
    set_visible(5);
    focus(row_keys[row]);
    draw_page();
}
static void choose_device(unsigned row) {
    if (more_devices && row==visible_choices) {
        first_choice+=visible_choices;
        device_choices();
        set_visible(visible_choices+more_devices);
        focus(row_keys[0]);
        draw_devices();
        return;
    }
    if (row>=visible_choices) return;
    const char *id=choices[first_choice+row];
    const char *other=draft.devices[1-device_seat];
    int shared_keyboard=(!strcmp(id,"Keyboard") || !strcmp(other,"Keyboard")) &&
                        !strncmp(id,"Key",3) && !strncmp(other,"Key",3);
    if (strcmp(id,"Auto") && (same_input(id,other) || shared_keyboard))
        strcpy(draft.devices[1-device_seat],draft.devices[device_seat]);
    strcpy(draft.devices[device_seat],id);
    end_devices();
}
static int open_page(void) {
    BYTE *pkg=package();
    if (!pkg) return 0;
    DWORD n=*(DWORD*)(pkg+0x88);
    BYTE **buttons=*(BYTE***)(pkg+0x84);
    if (!buttons || n!=10) return 0;
    BYTE *keep[10]={0};
    for (unsigned i=0;i<n;++i) {
        int row=row_number(object_key(buttons[i]));
        if (row>=0) keep[row]=buttons[i];
    }
    for (int i=0;i<10;++i) if (!keep[i]) return 0;
    load_settings();
    attach_input();
    enumerate_devices();
    draft=saved;
    original_count=n;
    memcpy(original_buttons,buttons,n*sizeof *buttons);
    for (unsigned i=0;i<n;++i) original_flags[i]=*(DWORD*)(buttons[i]+0x1c);
    page=pkg;
    set_visible(5);
    notice=NULL;
    focus(row_keys[0]);
    draw_page();
    next_poll=GetTickCount()+700;
    return 1;
}
static void close_page(void) {
    BYTE *pkg=page;
    page=NULL;
    page_owner=NULL;
    device_seat=-1;
    editing_seat=-1;
    BYTE **buttons=*(BYTE***)(pkg+0x84);
    memcpy(buttons,original_buttons,original_count*sizeof *buttons);
    *(DWORD*)(pkg+0x88)=original_count;
    for (unsigned i=0;i<original_count;++i) {
        BYTE *btn=buttons[i];
        DWORD *flags=(DWORD*)(btn+0x1c);
        *flags=(*flags&~3u)|(original_flags[i]&3)|0x400000;
    }
    BYTE *title=object(pkg,TITLE);
    if (title) *(DWORD*)(title+0x1c)=(*(DWORD*)(title+0x1c)&~2u)|0x400000;
    label(pkg,OPTIONS_BUTTON,"SPLIT SCREEN OPTIONS");
    focus(OPTIONS_BUTTON);
    label(pkg,DESCRIPTION,"Choose names and input devices for split screen.");
}
void split_device_tick(void) {
    if (!page) return;
    if (device_seat>=0) {
        static WORD previous[4];
        for (int i=0;i<4;++i) if (pads[i]) {
            XINPUT_STATE state={0};
            if (XInputGetState(i,&state)==ERROR_SUCCESS && (state.Gamepad.wButtons & ~previous[i])) {
                strcpy(identified,pad_ids[i]);
                page_dirty=1;
            }
            previous[i]=state.Gamepad.wButtons;
        }
    }
    if ((LONG)(GetTickCount()-next_poll)>=0) {
        enumerate_devices();
        if (device_seat>=0) {
            device_choices();
            unsigned shown=visible_choices+more_devices;
            if (shown!=*(DWORD*)(page+0x88)) {
                set_visible(shown);
                focus(row_keys[0]);
            }
        }
        next_poll=GetTickCount()+700;
        page_dirty=1;
    }
    if (page_dirty) draw_page();
}
typedef void (__attribute__((thiscall))*MenuEvent)(void*,unsigned,void*,void*,void*);
typedef void *(__attribute__((thiscall))*MenuDestructor)(void*,unsigned);
static void *__attribute__((fastcall)) destroy_options(void *screen,void *unused,unsigned flags) {
    if (screen==page_owner) {
        if (page && package()==page) {
            memcpy(*(BYTE***)(page+0x84),original_buttons,original_count*sizeof *original_buttons);
            *(DWORD*)(page+0x88)=original_count;
        }
        page=NULL;
        page_owner=NULL;
        editing_seat=device_seat=-1;
        ignore_until=0;
    }
    return ((MenuDestructor)0x4d42f0)(screen,flags);
}
static void __attribute__((fastcall)) main_event(void *screen,void *unused,unsigned msg,
                                                 BYTE *sender,void *a,void *b) {
    ((MenuEvent)0x4b3240)(screen,msg,sender,a,b);
    if (msg==0xabc08912 && sender)
        update_row_focus(find_package("MU_Main_PC.fng"),0x11793157,0x6dcb1d92,object_key(sender));
}
static void __attribute__((fastcall)) options_event(void *screen,void *unused,unsigned msg,
                                                   BYTE *sender,void *a,void *b) {
    const unsigned accept=0x0c407210, highlight=0xabc08912, back=0x911ab364;
    uint32_t key=sender && (msg==accept || msg==highlight)?object_key(sender):0;
    if (msg==highlight && sender) update_row_focus(package(),OPTIONS_BUTTON,0xc2ea87f8,key);
    if (page) {
        if (msg==0xc98356ba) { split_device_tick(); return; }
        if (msg==highlight) {page_dirty=1;return;}
        if (msg==accept || msg==back) {
            if (editing_seat>=0 || (ignore_until && (LONG)(ignore_until-GetTickCount())>0)) return;
            ignore_until=0;
            if (device_seat>=0) {
                if (msg==back) end_devices();
                else choose_device((unsigned)row_number(key));
                return;
            }
            if (msg==back) {close_page();return;}
            int row=row_number(key);
            notice=NULL;
            if (row==0 || row==2) {
                editing_seat=row/2;
                strcpy(editing_name,draft.names[editing_seat]);
                replace_name=1;
            } else if (row==1 || row==3) enter_devices(row/2);
            else if (row==4) {
                if (save_settings()) {close_page();return;}
                notice="Could not save SplitScreenOptions.ini. Try again.";
            }
            draw_page();
            return;
        }
    } else if (key==OPTIONS_BUTTON && msg==accept) {
        if (open_page()) page_owner=screen;
        return;
    }
    ((MenuEvent)0x4d4610)(screen,msg,sender,a,b);
    if (!page && (msg==0x35f8620b || (msg==highlight && key==OPTIONS_BUTTON))) {
        BYTE *pkg=package();
        if (pkg) {
            label(pkg,OPTIONS_BUTTON,"SPLIT SCREEN OPTIONS");
            if (msg==highlight) label(pkg,DESCRIPTION,"Choose names and input devices for split screen.");
        }
    }
}

/* Player records, race entries and the standings builder each read a name
   independently. Use the local seat across all three paths. */
static int direct_split;
static int local_race_names(void) {
    return *(int*)0x78a320 == 2 && (direct_split || *(int*)0x777b4c == 4);
}

__attribute__((used)) static const char *race_name(unsigned seat, const char *stock) {
    return seat < 2 && local_race_names() ? split_nickname(seat) : stock;
}
__attribute__((naked)) static void entry_name(void) {
    __asm__ volatile("cmpl $1,4(%esi)\n jne 1f\n"
                     "pushl %eax\n pushl %edx\n pushl %ecx\n movzbl 1(%esi),%eax\n pushl %eax\n"
                     "call _race_name\n addl $8,%esp\n movl %eax,%ecx\n popl %edx\n popl %eax\n"
                     "1: movl $16,%edi\n ret\n");
}

/* The HUD and results screens read player+0x34. The stock constructor
   overwrites this field from the profiles, even for a local two-seat race. */
__attribute__((used)) static int copy_local_name(char *destination, unsigned seat) {
    const char *name = race_name(seat, NULL);
    if (!name) return 0;
    strcpy(destination, name);
    return 1;
}
__attribute__((naked)) static void driver_name(void) {
    __asm__ volatile("pushal\n pushl %esi\n leal 0x34(%ebx),%eax\n pushl %eax\n"
                     "call _copy_local_name\n addl $8,%esp\n popal\n movl 0x7361c4,%eax\n ret\n");
}

/* This standings path independently looks up a profile by the viewing
   player. EDI is the race entry for the row, which survives place changes. */
__attribute__((naked)) static void standings_name(void) {
    __asm__ volatile("pushal\n movzbl 1(%edi),%eax\n pushl %eax\n pushl %esi\n call _copy_local_name\n"
                     "addl $8,%esp\n testl %eax,%eax\n popal\n jz 1f\n pushl $0x4a3a35\n ret\n"
                     "1: movl 0x24(%esp),%edx\n movl 0x10(%edx),%eax\n pushl $0x4a3a16\n ret\n");
}

int install_split_race_names(int direct) {
    struct NameSite { BYTE *address; const char *expected; size_t size; BYTE opcode; void (*hook)(void); };
    const struct NameSite sites[] = {
        {(BYTE*)0x580a1d, "\xbf\x10\x00\x00\x00", 5, 0xe8, entry_name},
        {(BYTE*)0x42b277, "\xa1\xc4\x61\x73\x00", 5, 0xe8, driver_name},
        {(BYTE*)0x4a3a0f, "\x8b\x54\x24\x24\x8b\x42\x10", 7, 0xe9, standings_name},
    };
    direct_split = direct;
    for (unsigned i=0; i<sizeof sites/sizeof sites[0]; ++i)
        if (memcmp(sites[i].address,sites[i].expected,sites[i].size)) return 0;
    for (unsigned i=0; i<sizeof sites/sizeof sites[0]; ++i) {
        const struct NameSite *site = &sites[i];
        DWORD old;
        if (!VirtualProtect(site->address,site->size,PAGE_EXECUTE_READWRITE,&old)) return 0;
        site->address[0] = site->opcode;
        int32_t offset = (uintptr_t)site->hook - (uintptr_t)site->address - 5;
        memcpy(site->address+1,&offset,4);
        memset(site->address+5,0x90,site->size-5);
        FlushInstructionCache(GetCurrentProcess(),site->address,site->size);
        VirtualProtect(site->address,site->size,old,&old);
    }
    return 1;
}
int install_split_options(void) {
    MenuEvent *main_dispatch=(MenuEvent*)0x6c6154;
    MenuEvent *dispatch=(MenuEvent*)0x6c3e54;
    MenuDestructor *destructor=(MenuDestructor*)0x6c3e4c;
    if (*main_dispatch!=(MenuEvent)0x4b3240 || *dispatch!=(MenuEvent)0x4d4610 ||
        *destructor!=(MenuDestructor)0x4d42f0) return 0;
    DWORD old;
    if (!VirtualProtect(main_dispatch,sizeof *main_dispatch,PAGE_READWRITE,&old)) return 0;
    *main_dispatch=(MenuEvent)main_event;
    VirtualProtect(main_dispatch,sizeof *main_dispatch,old,&old);
    if (!VirtualProtect(destructor,12,PAGE_READWRITE,&old)) return 0;
    *dispatch=(MenuEvent)options_event;
    *destructor=(MenuDestructor)destroy_options;
    VirtualProtect(destructor,12,old,&old);
    return 1;
}
