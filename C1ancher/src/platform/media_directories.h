#ifndef C1_MEDIA_DIRECTORIES_H
#define C1_MEDIA_DIRECTORIES_H

#include <stdbool.h>

#define C1_MEDIA_ROOT "/storage/mtp"
#define C1_PICTURES_DIRECTORY C1_MEDIA_ROOT "/Pic"
#define C1_MUSIC_DIRECTORY C1_MEDIA_ROOT "/Music"
#define C1_BOOKS_DIRECTORY C1_MEDIA_ROOT "/Book"

/* Open an absolute path without following any symlink; create only its leaf. */
int c1_media_open_directory(const char *directory, bool create);
/* Idempotent startup provisioning; preserve all existing files and directories. */
bool c1_media_directories_prepare_from(const char *root);

#endif
