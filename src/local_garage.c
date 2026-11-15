#define WIN32_LEAN_AND_MEAN
#include "local_garage.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

_Static_assert(sizeof(CarRow)==0x740 && offsetof(CarRow, key)==0x738, "PC car selector layout");
typedef struct {
    uint32_t used, slot_key, model;
    BYTE finish, reserved[3], part_type[120], group[120];
    uint32_t parts[120], upgrades[22];
} CarRecord;
_Static_assert(sizeof(CarRecord)==0x338, "PC profile car layout");
typedef struct { CarRecord car; int rating, extra[2], key; } Editable;
static Editable working;
static CarRecord original, last_preview;
static int editing, edit_key;
enum { BUILD=0x53000000, STORED=0x54000000, STOCK=0x55000000, KIND=0xff000000, INDEX=0xffffff };
static CarRecord *catalog;
static DWORD count;
static const char signature[8]={'N','F','S','U','G','A','R',1};
static wchar_t folder[MAX_PATH], file[MAX_PATH], temporary[MAX_PATH], lockfile[MAX_PATH];

static void pack(CarRecord *out, const void *ride) {
    memset(out,0,sizeof *out);
    ((void(WINAPI*)(CarRecord*,const void*))0x4ac750)(out,ride);
}
static void unpack(const CarRecord *car, void *ride) {
    void *target=ride;
    __asm__ volatile("movl $0x417600,%%eax;call *%%eax" : "+d"(target) : : "eax","ecx","cc","memory");
    ((void(WINAPI*)(const CarRecord*,int,void*))0x4ac5a0)(car,0,ride);
}
static CarRow *end(CarSelector *s) { return (CarRow*)&s->first; }
static void *model(int id) { return (BYTE*)*(void**)0x734588 + id*0xc90; }
static int model_id(const CarRow *r) { return *(const int*)r->ride; }
static int stock_key(int id) {
    void *info=model(id);
    return ((int(__cdecl*)(const char*,...))0x4f42f0)("CUSTOMIZABLE_%s",(char*)info);
}
static int is_added(int key) {
    unsigned kind=(unsigned)key & KIND;
    return kind==BUILD || kind==STOCK || kind==STORED;
}
int garage_base_key(const CarRow *row) {
    return is_added(row->key) ? stock_key(model_id(row)) : row->key;
}
static int valid(const CarRecord *c) {
    if(c->used!=1)return 0;
    int m=0;
    while(m<35 && *(uint32_t*)((BYTE*)model(m)+0xd0)!=c->model)++m;
    if(m==35)return 0;
    for(int i=0;i<120;++i)if(c->parts[i] && (c->group[i]>=32 || c->part_type[i]>=128))return 0;
    for(int i=0;i<22;++i)if(c->upgrades[i]>4)return 0;
    return 1;
}
static int paths(void) {
    if(file[0])return 1;
    wchar_t base[MAX_PATH];
    DWORD n=GetModuleFileNameW(NULL,base,MAX_PATH);
    if(!n || n>=MAX_PATH-48)return 0;
    wchar_t *slash=wcsrchr(base,L'\\');if(!slash)return 0;
    *slash=0;
    swprintf(folder,MAX_PATH,L"%ls\\save",base);
    swprintf(file,MAX_PATH,L"%ls\\SplitScreenGarage.bin",folder);
    swprintf(temporary,MAX_PATH,L"%ls.tmp",file);
    swprintf(lockfile,MAX_PATH,L"%ls.lock",file);
    return 1;
}
static void clear_catalog(void) {
    if(catalog)HeapFree(GetProcessHeap(),0,catalog);
    catalog=NULL; count=0;
}
static int load_catalog(void) {
    if(!paths())return 0;
    HANDLE h=CreateFileW(file,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    if(h==INVALID_HANDLE_VALUE) {
        DWORD err=GetLastError();
        if(err!=ERROR_FILE_NOT_FOUND && err!=ERROR_PATH_NOT_FOUND)return 0;
        clear_catalog();return 1;
    }
    LARGE_INTEGER size;
    char head[8]; DWORD got;
    int ok=GetFileSizeEx(h,&size) && size.QuadPart>=8 && size.QuadPart<=64*1024*1024 &&
           (size.QuadPart-8)%sizeof(CarRecord)==0 &&
           ReadFile(h,head,8,&got,NULL) && got==8 && !memcmp(head,signature,8);
    DWORD bytes=ok?(DWORD)size.QuadPart-8:0;
    CarRecord *items=NULL;
    if(ok && bytes){
        items=HeapAlloc(GetProcessHeap(),0,bytes);
        ok=items && ReadFile(h,items,bytes,&got,NULL) && got==bytes;
        for(unsigned i=0;ok && i<bytes/sizeof *items;++i)ok=valid(items+i);
    }
    CloseHandle(h);
    if(!ok){if(items)HeapFree(GetProcessHeap(),0,items);return 0;}
    clear_catalog();catalog=items;count=bytes/sizeof *items;return 1;
}
static int write_all(HANDLE f,const void *data,DWORD bytes){
    DWORD done;return !bytes || (WriteFile(f,data,bytes,&done,NULL) && done==bytes);
}
/* Serialize competing confirmations and publish only complete catalogs. */
static int store(const CarRecord *car) {
    if(!valid(car) || !paths())return 0;
    if(!CreateDirectoryW(folder,NULL) && GetLastError()!=ERROR_ALREADY_EXISTS)return 0;
    HANDLE lock=CreateFileW(lockfile,GENERIC_WRITE,0,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(lock==INVALID_HANDLE_VALUE)return 0;
    int key=0;
    if(load_catalog()){
        DWORD i=0;
        while(i<count && memcmp(catalog+i,car,sizeof *car))++i;
        if(i<count)key=BUILD+i+1;
        else if(8+(count+1)*sizeof *car<=64*1024*1024) {
            HANDLE f=CreateFileW(temporary,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
            if(f!=INVALID_HANDLE_VALUE){
                int ok=write_all(f,signature,8) && write_all(f,catalog,count*sizeof *car) &&
                       write_all(f,car,sizeof *car) && FlushFileBuffers(f);
                CloseHandle(f);
                if(ok && MoveFileExW(temporary,file,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
                    key=BUILD+i+1;
                else DeleteFileW(temporary);
            }
        }
    }
    CloseHandle(lock);return key;
}
static CarRow *find_key(CarSelector *s,int key){
    for(CarRow *r=s->first;r!=end(s);r=r->next)if(r->key==key)return r;
    return NULL;
}
static CarRow *find_record(CarSelector *s,const CarRecord *c,CarRow *skip){
    for(CarRow *r=s->first;r!=end(s);r=r->next){
        if(r==skip || *(uint32_t*)((BYTE*)r->model_info+0xd0)!=c->model)continue;
        CarRecord cmp;pack(&cmp,r->ride);
        if(!memcmp(&cmp,c,sizeof cmp))return r;
    }
    return NULL;
}
static CarRow *append(CarSelector *s,const CarRecord *c,int key){
    if(!valid(c))return NULL;
    CarRow *r=((void*(__cdecl*)(size_t))0x67121a)(sizeof *r);
    if(!r)return NULL;
    memset(r,0,sizeof *r);
    unpack(c,r->ride);
    r->model_info=model(model_id(r));r->rating=*(int*)(r->ride+0x428);r->key=key;
    r->prev=s->last;r->next=end(s);s->last->next=r;s->last=r;
    return r;
}
__attribute__((naked)) static void factory(Editable *out,int id,int key){
    __asm__ volatile("pushl %ebx\n movl 8(%esp),%ebx\n movl 12(%esp),%ecx\n movl 16(%esp),%eax\n"
                     "movl $0x4acb90,%edx\n call *%edx\n popl %ebx\n ret\n");
}
static void add_logo(CarSelector *s,CarRow *r){
    void *browser=(BYTE*)s+0x50;
    BYTE *image=((BYTE*(WINAPI*)(void*,int,int,int))0x4f8730)(browser,r->key,0,1);
    if(image){
        *(int*)(image+0x24)=((int(__cdecl*)(const char*,...))0x4f42f0)
            ("CARSELECT_MANUFACTURER_%s",(char*)r->model_info+0xc0);
        *(int*)(image+0x1c)|=0x400000;
    }
}
static void load_row(CarRow *row,const CarRecord *record){
    unpack(record,row->ride);
    row->model_info=model(model_id(row));
    row->rating=*(int*)(row->ride+0x428);
}
/* Keep the horizontal browser, the 3D preview and the selected list node together. */
__attribute__((naked)) static void focus_car(CarSelector *s,CarRow *r){
    __asm__ volatile("pushl %esi\n movl 12(%esp),%eax\n movl 0x738(%eax),%esi\n"
        "addl $0x10,%eax\n pushl $1\n pushl %eax\n xorl %eax,%eax\n"
        "movl $0x4b23b0,%ecx\n call *%ecx\n addl $8,%esp\n movl 8(%esp),%edx\n"
        "addl $0x50,%edx\n movl $0x4f8910,%eax\n call *%eax\n popl %esi\n ret\n");
}
void garage_focus(CarSelector *s,int key){
    CarRow *r=find_key(s,key);
    if(r){
        s->selected=r;
        *(int*)0x758c28=r->key;
        if(*(void**)((BYTE*)s+0x0c))focus_car(s,r);
    }
}
void garage_populate(CarSelector *s){
    if(!load_catalog())clear_catalog();
    CarRow *last=s->last;
    for(CarRow *r=s->first;r!=end(s);r=r->next){
        int id=model_id(r);
        if(r->key==stock_key(id)){
            Editable stock={0};CarRecord current;
            factory(&stock,id,r->key);stock.car.slot_key=0;
            pack(&current,r->ride);
            if(memcmp(&stock.car,&current,sizeof current))append(s,&stock.car,STOCK+id);
        }
        if(r==last)break;
    }
    for(int i=0;i<10;++i){
        const CarRecord *c=(const CarRecord*)0x758c30+i;
        if(c->used && valid(c))append(s,c,STORED+i);
    }
    for(unsigned i=0;i<count;++i)append(s,catalog+i,BUILD+i+1);
    if(editing){
        CarRow *r=find_key(s,edit_key);
        if(r){load_row(r,&working.car);last_preview=working.car;*(int*)0x758c28=edit_key;}
    }
}
void *garage_edit(CarSelector *s){
    CarRow *r=s->selected;
    if(!editing || edit_key!=r->key){
        pack(&original,r->ride);
        working=(Editable){0};working.car=original;working.rating=r->rating;
        working.key=garage_base_key(r);edit_key=r->key;editing=1;
    }
    last_preview=working.car;
    return &working;
}
void garage_refresh(CarSelector *s){
    if(editing && s->selected && s->selected->key==edit_key &&
       memcmp(&last_preview,&working.car,sizeof last_preview)){
        load_row(s->selected,&working.car);last_preview=working.car;
    }
}
void garage_discard(CarSelector *s){
    if(editing){
        CarRow *r=find_key(s,edit_key);
        if(r)load_row(r,&original);
        editing=0;
    }
}
int garage_confirm(CarSelector *s){
    garage_refresh(s);
    CarRow *r=s->selected;
    if(!editing || r->key!=edit_key || !memcmp(&original,&working.car,sizeof original)){
        garage_discard(s);return 1;
    }
    CarRecord confirmed;pack(&confirmed,r->ride);
    CarRow *target=find_record(s,&confirmed,r);
    if(!target){
        int key=store(&confirmed);
        if(!key)return 0;
        target=find_key(s,key);
        if(!target){target=append(s,&confirmed,key);if(!target)return 0;add_logo(s,target);}
    }
    garage_discard(s);
    garage_focus(s,target->key);
    return 1;
}
void garage_scrolled(CarSelector *s){
    if(editing && s->selected && s->selected->key!=edit_key)garage_discard(s);
}
void *garage_load_manual(const void *ride){
    if(!editing || *(void**)0x744ff8!=&working)return NULL;
    pack(&working.car,ride);
    working.key=stock_key(*(const int*)ride);
    return &working;
}
void garage_leave(void){
    editing=0;clear_catalog();
}
void garage_title(CarSelector *s,int player,char *text,unsigned size){
    CarRow *r=s?s->selected:NULL;
    if(!r){snprintf(text,size,"PLAYER %d - CHOOSE YOUR CAR",player);return;}
    int key=r->key,kind=key & KIND;
    const char *name=r->model_info;
    if(kind==BUILD || kind==STORED)
        snprintf(text,size,"PLAYER %d - %.32s - %s %u",player,name,
            kind==BUILD?"BUILD":"CUSTOM SLOT",(key & INDEX)+(kind==STORED));
    else if(key==stock_key(model_id(r)) && find_key(s,STOCK+model_id(r)))
        snprintf(text,size,"PLAYER %d - %.32s - PROFILE CAR",player,name);
    else if(kind==STOCK || key==stock_key(model_id(r)))snprintf(text,size,"PLAYER %d - %.32s - STOCK",player,name);
    else snprintf(text,size,"PLAYER %d - CHOOSE YOUR CAR",player);
}
