#ifndef C1_CORE_RECORD_H
#define C1_CORE_RECORD_H

#include "core/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define C1_RECORD_FIELD_CAPACITY 12

typedef enum {
    C1_FIELD_TEXT = 0,
    C1_FIELD_INTEGER,
    C1_FIELD_BOOLEAN
} c1_field_type;

typedef struct {
    const char *name;
    c1_field_type type;
    union {
        const char *text;
        int64_t integer;
        bool boolean;
    } value;
} c1_field;

typedef struct {
    const char *category;
    const char *event;
    c1_field fields[C1_RECORD_FIELD_CAPACITY];
    size_t field_count;
} c1_record;

typedef c1_status (*c1_record_emit_fn)(void *context, const c1_record *record);

typedef struct {
    c1_record_emit_fn emit;
    void *context;
} c1_record_sink;

void c1_record_init(c1_record *record, const char *category, const char *event);
c1_status c1_record_add_text(c1_record *record, const char *name, const char *value);
c1_status c1_record_add_integer(c1_record *record, const char *name, int64_t value);
c1_status c1_record_add_boolean(c1_record *record, const char *name, bool value);
c1_status c1_record_emit(c1_record_sink sink, const c1_record *record);
c1_record_sink c1_ndjson_sink(FILE *stream);

#endif