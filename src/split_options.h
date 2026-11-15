#ifndef SPLIT_OPTIONS_H
#define SPLIT_OPTIONS_H
#include <windows.h>

enum { SPLIT_NAME_BYTES = 16, PORT_KEYBOARD = -1, PORT_RAW_KEYBOARD = -2, PORT_UNAVAILABLE = -3 };
const char *split_nickname(int seat);
void split_assign_devices(int ports[2]);
BYTE split_key(int seat, int port, unsigned vk);
void split_device_tick(void);
int install_split_race_names(int direct);
int install_split_options(void);
#endif
