#ifndef C1_UPDATE_REPOSITORY_H
#define C1_UPDATE_REPOSITORY_H

#include "update/update.h"

#include <stddef.h>
#include <stdint.h>

#define C1_UPDATE_URL_MAX 2048U
#define C1_UPDATE_MANIFEST_NAME "manifest.v1"
#define C1_UPDATE_SIGNATURE_NAME "manifest.v1.sig"

struct c1_update_release {
    struct c1_update_manifest manifest;
    unsigned char *manifest_data;
    size_t manifest_size;
    unsigned char signature[64];
};

/* Missing profiles use C1_UPDATE_DEFAULT_REPOSITORY. Existing profiles must
 * pass the same ownership, permissions and URL validation as before. */
int c1_update_repository_read_url(const char *path, char *url, size_t url_size,
                                  char *error, size_t error_size);
int c1_update_repository_validate_url(const char *base_url,
                                      char *error, size_t error_size);
/* HTTP origins cannot redirect to HTTPS. Only the deployment's exact domain
 * has a fixed-IP second attempt; other configured repositories remain isolated.
 * This checks length; the transaction verifies the signed component SHA-256. */
int c1_update_repository_download(const char *base_url, const char *relative_path,
                                  const char *destination, uint64_t maximum_size,
                                  uint64_t exact_size, char *error, size_t error_size);
int c1_update_repository_load_local(const char *release_directory, const char *key_path,
                                    struct c1_update_release *release,
                                    char *error, size_t error_size);
/* Fetch and verify a complete signed pair per origin, with a shared 45-second
 * metadata transfer budget. Failed verification retries the pair only once at
 * the fixed IP; sequence/security-epoch checks remain in the transaction. */
int c1_update_repository_fetch_release(const char *base_url, const char *staging_directory,
                                       const char *key_path, struct c1_update_release *release,
                                       char *error, size_t error_size);
void c1_update_repository_release_free(struct c1_update_release *release);

#endif