#include "pkg.h"

#include <string.h>

int c1pkg_app_uses_direct_io(const char *id)
{
    if (id == NULL) {
        return 0;
    }
    return strcmp(id, "chichugames") == 0 ||
           strcmp(id, "hello") == 0 ||
           strcmp(id, "book-reader") == 0 ||
           strcmp(id, "music-player") == 0 ||
           strcmp(id, "pic") == 0;
}