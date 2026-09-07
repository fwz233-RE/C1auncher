#ifndef C1PKG_GUI_H
#define C1PKG_GUI_H
#include "pkg.h"

int c1pkg_gui(const struct c1pkg_config *config);
/* Internal: caller must own the application run lock for the whole call. */
int c1pkg_store_remove_with_run_lock(const char *id, int owned_run_lock,
                                     char *error, size_t error_size);
#endif
