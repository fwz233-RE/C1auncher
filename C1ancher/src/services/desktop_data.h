#ifndef C1_DESKTOP_DATA_H
#define C1_DESKTOP_DATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define C1_DESKTOP_BODY_MAX 16384U
#define C1_DESKTOP_PACKAGES 128U
#define C1_DESKTOP_QUOTE_BYTES 193U
#define C1_DESKTOP_BANNER_WIDTH 284
#ifndef C1_DESKTOP_CACHE
#define C1_DESKTOP_CACHE "/usr/data/c1/desktop-summary.cache"
#endif
#ifndef C1_DESKTOP_SEEN
#define C1_DESKTOP_SEEN "/usr/data/c1/desktop-seen.cache"
#endif

typedef struct {
    char source[1025];
    char date[11];
    /* C1DESKTOP 1 retains its two wire/cache slots. quote_zh is the original
     * in any language; quote_en is compatibility-only, never display locale.
     * Current servers write the same original into both slots. */
    char quote_zh[C1_DESKTOP_QUOTE_BYTES], quote_en[C1_DESKTOP_QUOTE_BYTES];
    uint64_t sequence;
    uint32_t count;
    char ids[C1_DESKTOP_PACKAGES][33];
} c1_desktop_data;

/* Optional, untrusted display metadata: never authorizes installation. */
bool c1_desktop_parse(const char *data, size_t size, c1_desktop_data *out);
bool c1_desktop_load(const char *path, c1_desktop_data *out);
bool c1_desktop_save(const char *path, const c1_desktop_data *data);
unsigned c1_desktop_new_count(const c1_desktop_data *current, const c1_desktop_data *seen);
bool c1_desktop_quote_valid(const char *quote);
#endif
