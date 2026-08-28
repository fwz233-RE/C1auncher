#include "hal/linux/display.h"

#include "display/frame.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#define C1_EPAPER_DEVICE "/dev/epaper_lcd"
#define C1_EPAPER_REFRESH "/sys/devices/platform/e0266a128/epaper/refresh"

static uint8_t cached_frame[C1_DISPLAY_FRAME_BYTES];
static bool cached_frame_valid;
static c1_linux_display_stats display_stats;

static bool write_refresh(int *error_number)
{
    static const char value[] = "1";
    int descriptor = open(C1_EPAPER_REFRESH, O_WRONLY | O_CLOEXEC);
    ssize_t written;

    if (descriptor < 0) {
        *error_number = errno;
        return false;
    }
    written = write(descriptor, value, sizeof(value) - 1U);
    if (written != (ssize_t)(sizeof(value) - 1U)) {
        *error_number = written < 0 ? errno : EIO;
        close(descriptor);
        return false;
    }
    if (close(descriptor) != 0) {
        *error_number = errno;
        return false;
    }
    return true;
}

static c1_status emit_result(c1_record_sink sink,
                             bool frame_written,
                             bool refresh_requested,
                             bool unchanged_skipped,
                             int write_error,
                             int refresh_error)
{
    c1_record record;

    c1_record_init(&record, "display", "frame_write");
    if (c1_record_add_integer(&record, "width", C1_DISPLAY_WIDTH) != C1_STATUS_OK ||
        c1_record_add_integer(&record, "height", C1_DISPLAY_HEIGHT) != C1_STATUS_OK ||
        c1_record_add_integer(&record, "bytes", C1_DISPLAY_FRAME_BYTES) != C1_STATUS_OK ||
        c1_record_add_boolean(&record, "frame_written", frame_written) != C1_STATUS_OK ||
        c1_record_add_boolean(&record, "refresh_requested", refresh_requested) != C1_STATUS_OK ||
        c1_record_add_boolean(&record, "unchanged_skipped", unchanged_skipped) != C1_STATUS_OK ||
        c1_record_add_integer(&record, "write_errno", write_error) != C1_STATUS_OK ||
        c1_record_add_integer(&record, "refresh_errno", refresh_error) != C1_STATUS_OK) {
        return C1_STATUS_INVALID_ARGUMENT;
    }
    return c1_record_emit(sink, &record);
}

static c1_status write_frame(const uint8_t *frame,
                             uint32_t size,
                             bool request_refresh,
                             c1_record_sink sink)
{
    int descriptor;
    ssize_t written;
    int write_error = 0;
    int refresh_error = 0;
    bool frame_written = false;
    bool refresh_requested = false;
    c1_status emit_status;

    if (frame == NULL || size != C1_DISPLAY_FRAME_BYTES || sink.emit == NULL) {
        return C1_STATUS_INVALID_ARGUMENT;
    }
    if (!request_refresh && cached_frame_valid &&
        memcmp(cached_frame, frame, C1_DISPLAY_FRAME_BYTES) == 0) {
        ++display_stats.unchanged_skips;
        return emit_result(sink, false, false, true, 0, 0);
    }

    descriptor = open(C1_EPAPER_DEVICE, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) {
        write_error = errno;
    } else {
        written = write(descriptor, frame, size);
        if (written == (ssize_t)size) {
            frame_written = true;
        } else {
            write_error = written < 0 ? errno : EIO;
        }
        if (close(descriptor) != 0 && frame_written) {
            frame_written = false;
            write_error = errno;
        }
    }

    if (frame_written && request_refresh) {
        refresh_requested = write_refresh(&refresh_error);
    }

    if (frame_written) {
        memcpy(cached_frame, frame, C1_DISPLAY_FRAME_BYTES);
        cached_frame_valid = true;
        ++display_stats.writes;
        if (request_refresh && refresh_requested) {
            ++display_stats.full_refreshes;
        }
    }
    emit_status = emit_result(sink,
                              frame_written,
                              refresh_requested,
                              false,
                              write_error,
                              refresh_error);
    if (emit_status != C1_STATUS_OK) {
        return emit_status;
    }
    return frame_written && (!request_refresh || refresh_requested) ? C1_STATUS_OK : C1_STATUS_IO_ERROR;
}

c1_status c1_linux_display_write_frame(void *context,
                                       const uint8_t *frame,
                                       uint32_t size,
                                       c1_record_sink sink)
{
    (void)context;
    return write_frame(frame, size, true, sink);
}

c1_status c1_linux_display_write_frame_fast(void *context,
                                            const uint8_t *frame,
                                            uint32_t size,
                                            c1_record_sink sink)
{
    (void)context;
    return write_frame(frame, size, false, sink);
}

void c1_linux_display_get_stats(c1_linux_display_stats *stats)
{
    if (stats != NULL) {
        *stats = display_stats;
    }
}

void c1_linux_display_reset_cache(void)
{
    cached_frame_valid = false;
}