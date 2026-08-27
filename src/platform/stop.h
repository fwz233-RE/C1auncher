#ifndef C1_PLATFORM_STOP_H
#define C1_PLATFORM_STOP_H

#include <stdbool.h>

void c1_stop_install(void);
bool c1_stop_requested(void);
void c1_stop_reset(void);

#endif