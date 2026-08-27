#ifndef C1_HAL_LINUX_DISPLAY_H
#define C1_HAL_LINUX_DISPLAY_H

#include "core/record.h"
#include "core/status.h"

#include <stdint.h>

c1_status c1_linux_display_write_frame(void *context,
                                       const uint8_t *frame,
                                       uint32_t size,
                                       c1_record_sink sink);
c1_status c1_linux_display_write_frame_fast(void *context,
                                            const uint8_t *frame,
                                            uint32_t size,
                                            c1_record_sink sink);

#endif