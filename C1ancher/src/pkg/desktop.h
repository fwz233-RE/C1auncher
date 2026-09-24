#ifndef C1PKG_DESKTOP_H
#define C1PKG_DESKTOP_H
#include "pkg.h"
int c1pkg_desktop_source(const char *input, char *out, size_t capacity);
int c1pkg_desktop_summary(const struct c1pkg_config *config);
void c1pkg_desktop_seen(const struct c1pkg_config *config, const struct c1pkg_index *index);
#endif
