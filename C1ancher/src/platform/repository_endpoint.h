#ifndef C1_REPOSITORY_ENDPOINT_H
#define C1_REPOSITORY_ENDPOINT_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* A fixed, trusted alternate origin for this deployment. Never derive an IP
 * from untrusted redirects/DNS and never change the downloaded signed path.
 * Other repositories deliberately have no implicit fallback. */
#define C1_REPOSITORY_PRIMARY_ORIGIN "http://www.fwz233.com"
#define C1_REPOSITORY_FALLBACK_ORIGIN "http://123.56.214.77"

static inline int c1_repository_fallback_url(const char *primary, char *output, size_t size)
{
    const char *suffix;
    int written;
    size_t length = strlen(C1_REPOSITORY_PRIMARY_ORIGIN);
    if (primary == NULL || output == NULL || size == 0U ||
        strncmp(primary, C1_REPOSITORY_PRIMARY_ORIGIN, length) != 0) return 0;
    suffix = primary + length;
    if (strncmp(suffix, ":80/", 4U) == 0) suffix += 3;
    if (*suffix != '/') return 0;
    written = snprintf(output, size, "%s%s", C1_REPOSITORY_FALLBACK_ORIGIN, suffix);
    return written >= 0 && (size_t)written < size ? 1 : 0;
}

#endif
