#define _DEFAULT_SOURCE 1
#include "pkg/desktop.h"
#include "services/desktop_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int c1pkg_desktop_source(const char *input, char *out, size_t capacity)
{
    if (!input || (strncmp(input, "http://", 7) && strncmp(input, "https://", 8))) return -1;
    size_t n = strnlen(input, capacity);
    if (!n || n >= capacity) return -1;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (c <= 32 || c >= 127 || strchr("?#@%[]{}\\", c)) return -1;
    }
    while (n && input[n - 1] == '/') --n;
    memcpy(out, input, n); out[n] = 0;
    if (n >= 6 && (!strcmp(out + n - 6, "/c1/v1") || !strcmp(out + n - 6, "/c1/v2"))) out[n - 1] = '2';
    return 0;
}

static int id_compare(const void *a, const void *b) { return strcmp(a, b); }
void c1pkg_desktop_seen(const struct c1pkg_config *config, const struct c1pkg_index *index)
{
    c1_desktop_data data = {0}, old;
    if (!config || !index || index->count > C1_DESKTOP_PACKAGES || c1pkg_desktop_source(config->repo_base, data.source, sizeof(data.source))) return;
    strcpy(data.date, "2026-01-01");
    strcpy(data.quote_zh, "Live free or die.");
    strcpy(data.quote_en, "Live free or die.");
    data.sequence = index->sequence;
    for (unsigned i = 0; i < index->count; ++i) {
        if (c1pkg_is_internal_id(index->packages[i].id)) continue;
        snprintf(data.ids[data.count], sizeof(data.ids[data.count]), "%s", index->packages[i].id);
        ++data.count;
    }
    qsort(data.ids, data.count, sizeof(data.ids[0]), id_compare);
    if (c1_desktop_load(C1_DESKTOP_SEEN, &old) && !strcmp(old.source, data.source) && old.sequence > data.sequence) return;
    (void)c1_desktop_save(C1_DESKTOP_SEEN, &data);
}
