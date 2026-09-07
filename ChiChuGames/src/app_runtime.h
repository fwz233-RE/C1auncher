#ifndef CCG_APP_RUNTIME_H
#define CCG_APP_RUNTIME_H

#include <stdbool.h>

int app_runtime_install_signals(void);
bool app_runtime_checkpoint(void);

#endif
