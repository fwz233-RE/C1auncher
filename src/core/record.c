#include "core/record.h"

#include <inttypes.h>

static c1_status add_field(c1_record *record, const char *name, c1_field_type type, c1_field **field)
{
    if (record == NULL || name == NULL || field == NULL || record->field_count >= C1_RECORD_FIELD_CAPACITY) {
        return C1_STATUS_INVALID_ARGUMENT;
    }

    *field = &record->fields[record->field_count++];
    (*field)->name = name;
    (*field)->type = type;
    return C1_STATUS_OK;
}

static bool write_json_text(FILE *stream, const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;

    if (fputc('"', stream) == EOF) {
        return false;
    }

    while (*cursor != '\0') {
        switch (*cursor) {
        case '"':
            if (fputs("\\\"", stream) == EOF) {
                return false;
            }
            break;
        case '\\':
            if (fputs("\\\\", stream) == EOF) {
                return false;
            }
            break;
        case '\b':
            if (fputs("\\b", stream) == EOF) {
                return false;
            }
            break;
        case '\f':
            if (fputs("\\f", stream) == EOF) {
                return false;
            }
            break;
        case '\n':
            if (fputs("\\n", stream) == EOF) {
                return false;
            }
            break;
        case '\r':
            if (fputs("\\r", stream) == EOF) {
                return false;
            }
            break;
        case '\t':
            if (fputs("\\t", stream) == EOF) {
                return false;
            }
            break;
        default:
            if (*cursor < 0x20U) {
                if (fprintf(stream, "\\u%04x", (unsigned int)*cursor) < 0) {
                    return false;
                }
            } else if (fputc((int)*cursor, stream) == EOF) {
                return false;
            }
            break;
        }
        ++cursor;
    }

    return fputc('"', stream) != EOF;
}

static c1_status ndjson_emit(void *context, const c1_record *record)
{
    FILE *stream = context;
    size_t index;

    if (stream == NULL || record == NULL || record->category == NULL || record->event == NULL) {
        return C1_STATUS_INVALID_ARGUMENT;
    }

    if (fputc('{', stream) == EOF || !write_json_text(stream, "category") || fputc(':', stream) == EOF ||
        !write_json_text(stream, record->category) || fputc(',', stream) == EOF ||
        !write_json_text(stream, "event") || fputc(':', stream) == EOF ||
        !write_json_text(stream, record->event)) {
        return C1_STATUS_IO_ERROR;
    }

    for (index = 0; index < record->field_count; ++index) {
        const c1_field *field = &record->fields[index];

        if (fputc(',', stream) == EOF || !write_json_text(stream, field->name) || fputc(':', stream) == EOF) {
            return C1_STATUS_IO_ERROR;
        }

        switch (field->type) {
        case C1_FIELD_TEXT:
            if (!write_json_text(stream, field->value.text)) {
                return C1_STATUS_IO_ERROR;
            }
            break;
        case C1_FIELD_INTEGER:
            if (fprintf(stream, "%" PRId64, field->value.integer) < 0) {
                return C1_STATUS_IO_ERROR;
            }
            break;
        case C1_FIELD_BOOLEAN:
            if (fputs(field->value.boolean ? "true" : "false", stream) == EOF) {
                return C1_STATUS_IO_ERROR;
            }
            break;
        }
    }

    if (fputs("}\n", stream) == EOF || fflush(stream) != 0) {
        return C1_STATUS_IO_ERROR;
    }

    return C1_STATUS_OK;
}

void c1_record_init(c1_record *record, const char *category, const char *event)
{
    record->category = category;
    record->event = event;
    record->field_count = 0;
}

c1_status c1_record_add_text(c1_record *record, const char *name, const char *value)
{
    c1_field *field;
    c1_status status;

    if (value == NULL) {
        return C1_STATUS_INVALID_ARGUMENT;
    }

    status = add_field(record, name, C1_FIELD_TEXT, &field);
    if (status == C1_STATUS_OK) {
        field->value.text = value;
    }
    return status;
}

c1_status c1_record_add_integer(c1_record *record, const char *name, int64_t value)
{
    c1_field *field;
    c1_status status = add_field(record, name, C1_FIELD_INTEGER, &field);

    if (status == C1_STATUS_OK) {
        field->value.integer = value;
    }
    return status;
}

c1_status c1_record_add_boolean(c1_record *record, const char *name, bool value)
{
    c1_field *field;
    c1_status status = add_field(record, name, C1_FIELD_BOOLEAN, &field);

    if (status == C1_STATUS_OK) {
        field->value.boolean = value;
    }
    return status;
}

c1_status c1_record_emit(c1_record_sink sink, const c1_record *record)
{
    if (sink.emit == NULL) {
        return C1_STATUS_INVALID_ARGUMENT;
    }
    return sink.emit(sink.context, record);
}

c1_record_sink c1_ndjson_sink(FILE *stream)
{
    c1_record_sink sink = {ndjson_emit, stream};
    return sink;
}
