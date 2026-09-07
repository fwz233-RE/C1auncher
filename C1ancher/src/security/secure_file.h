#ifndef C1_SECURE_FILE_H
#define C1_SECURE_FILE_H

#include <stddef.h>
#include <sys/types.h>

#define C1_SECURE_FILE_ANY_SIZE ((size_t)-1)

void c1_secure_set_error(char *error, size_t error_size, const char *format, ...);
int c1_secure_read_file(const char *path, unsigned char **data, size_t *size,
                        size_t maximum_size, size_t exact_size,
                        char *error, size_t error_size);
int c1_secure_atomic_write(const char *path, const void *data, size_t size,
                           mode_t mode, char *error, size_t error_size);
int c1_secure_sync_directory(const char *path, char *error, size_t error_size);

#endif