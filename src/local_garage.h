#ifndef LOCAL_GARAGE_H
#define LOCAL_GARAGE_H
#include <windows.h>
#include <stdint.h>

typedef struct CarRow {
    struct CarRow *next, *prev;
    BYTE pad[8], ride[0x720];
    void *model_info;
    int rating, key, unused;
} CarRow;
typedef struct {
    BYTE pad[0x40];
    CarRow *first, *last, *selected, *initial;
} CarSelector;

void garage_populate(CarSelector *screen);
void *garage_edit(CarSelector *screen);
void garage_refresh(CarSelector *screen);
int garage_confirm(CarSelector *screen);
void garage_discard(CarSelector *screen);
int garage_base_key(const CarRow *row);
void garage_title(CarSelector *screen, int player, char *text, unsigned size);
void garage_focus(CarSelector *screen,int key);
void garage_scrolled(CarSelector *screen);
void *garage_load_manual(const void *ride);
void garage_leave(void);
#endif
