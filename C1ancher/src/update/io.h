#ifndef C1_UPDATE_IO_H
#define C1_UPDATE_IO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

int c1_update_join_path(char *out, size_t out_size, const char *left, const char *right);
int c1_update_check_trusted_directory(const char *path, dev_t expected_device,
                                      dev_t *device, char *error, size_t error_size);
int c1_update_make_directory(const char *path, mode_t mode, dev_t expected_device,
                             int allow_existing, char *error, size_t error_size);
int c1_update_check_space(const char *path, uint64_t required,
                          char *error, size_t error_size);
int c1_update_copy_file(const char *source, const char *destination,
                        uint64_t expected_size, const char *expected_sha256,
                        mode_t final_mode, char *error, size_t error_size);
int c1_update_remove_tree(const char *path);
int c1_update_lock(const char *root, char *lock_path, size_t lock_path_size,
                   char *error, size_t error_size);
void c1_update_unlock(int descriptor, const char *lock_path);

#endif