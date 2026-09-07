#ifndef C1_UI_WALLPAPER_H
#define C1_UI_WALLPAPER_H

#include <stdbool.h>
#include <stdint.h>

#include "display/frame.h"

#include "platform/media_directories.h"

#define C1_WALLPAPER_DIRECTORY C1_PICTURES_DIRECTORY
#define C1_WALLPAPER_FILENAME "wallpaper.raw"

extern uint8_t c1_wallpaper_frame[C1_DISPLAY_FRAME_BYTES];

/* Load the fixed raw frame, provisioning the original default when absent.
 * An unreadable file/storage uses the built-in default without overwriting it. */
void c1_wallpaper_load(void);

/* Injectable directory variant; returns whether a complete on-disk frame is available. */
bool c1_wallpaper_load_or_default_from(const char *directory, const char *legacy);

/* Read-only raw loader. */
bool c1_wallpaper_load_from(const char *directory);

#endif
