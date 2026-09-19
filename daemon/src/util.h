#ifndef OMA_UTIL_H
#define OMA_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Resolve a file under one of the XDG base directories, creating the parent
 * directories along the way. `leaf` is relative, e.g. "config.json".
 * Returns 0 on success, -1 with errno set otherwise. */
int oma_path_config(char *buf, size_t len, const char *leaf);
int oma_path_data(char *buf, size_t len, const char *leaf);
int oma_path_state(char *buf, size_t len, const char *leaf);
int oma_path_runtime(char *buf, size_t len, const char *leaf);

/* mkdir -p. Existing directories are not an error. */
int oma_mkdir_p(const char *path, mode_t mode);

/* Write to "<path>.tmp" then rename over `path`, so a reader never sees a
 * half-written file and a crash never truncates the old one. */
int oma_write_atomic(const char *path, const char *data, size_t len, mode_t mode);

/* Read a whole file into a NUL-terminated heap buffer. Caller frees.
 * Returns NULL on error; *out_len, when given, gets the byte count. */
char *oma_read_file(const char *path, size_t *out_len);

/* "<prefix>" + seconds since the epoch + 4 random bytes, all hex. Unique
 * enough for contact ids without pulling in a UUID dependency. */
void oma_id_new(char *buf, size_t len, const char *prefix);

/* Seconds since the epoch. */
int64_t oma_now(void);

/* strlcpy, which glibc does not ship. Always NUL-terminates; returns the
 * length of `src`, so a truncation shows up as a return >= size. */
size_t oma_strlcpy(char *dst, const char *src, size_t size);

#endif /* OMA_UTIL_H */
