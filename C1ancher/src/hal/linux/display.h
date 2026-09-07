#ifndef C1_HAL_LINUX_DISPLAY_H
#define C1_HAL_LINUX_DISPLAY_H

#include "core/record.h"
#include "core/status.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t writes;
    uint64_t full_refreshes;
    uint64_t unchanged_skips;
    uint64_t lease_skips;
} c1_linux_display_stats;

c1_status c1_linux_display_write_frame(void *context,
                                       const uint8_t *frame,
                                       uint32_t size,
                                       c1_record_sink sink);
c1_status c1_linux_display_write_frame_fast(void *context,
                                            const uint8_t *frame,
                                            uint32_t size,
                                            c1_record_sink sink);
void c1_linux_display_get_stats(c1_linux_display_stats *stats);
void c1_linux_display_reset_cache(void);

#endif