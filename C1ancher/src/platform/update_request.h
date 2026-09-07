#ifndef C1_PLATFORM_UPDATE_REQUEST_H
#define C1_PLATFORM_UPDATE_REQUEST_H

#include <stddef.h>

#ifndef C1_UPDATE_REQUEST_DEFAULT_PATH
#define C1_UPDATE_REQUEST_DEFAULT_PATH "/usr/data/c1/update/restart-request"
#endif
#define C1_UPDATE_REQUEST_DIGEST_SIZE 64U
#define C1_UPDATE_REQUEST_FILE_SIZE 65U

int c1_update_request_validate_digest(const char *digest);
int c1_update_request_write(const char *path, const char *digest,
                            char *error, size_t error_size);
int c1_update_request_read(const char *path, char digest[65],
                           char *error, size_t error_size);
int c1_update_request_consume(const char *path, const char *expected_digest,
                              char *error, size_t error_size);

#endif