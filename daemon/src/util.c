#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

size_t oma_strlcpy(char *dst, const char *src, size_t size)
{
    size_t srclen = strlen(src);

    if (size > 0) {
        size_t copy = srclen >= size ? size - 1 : srclen;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return srclen;
}

int64_t oma_now(void)
{
    return (int64_t)time(NULL);
}

int oma_mkdir_p(const char *path, mode_t mode)
{
    char buf[512];
    size_t len = oma_strlcpy(buf, path, sizeof(buf));
    char *p;

    if (len >= sizeof(buf)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    for (p = buf + 1; *p != '\0'; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, mode) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(buf, mode) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* Join `base`/omacall/`leaf` into `buf` and make sure the directory exists. */
static int path_under(char *buf, size_t len, const char *base, const char *leaf)
{
    char dir[512];
    int n;

    n = snprintf(dir, sizeof(dir), "%s/omacall", base);
    if (n < 0 || (size_t)n >= sizeof(dir)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (oma_mkdir_p(dir, 0700) != 0)
        return -1;

    n = snprintf(buf, len, "%s/%s", dir, leaf);
    if (n < 0 || (size_t)n >= len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

/* $VAR when set and absolute, else $HOME/`fallback`. */
static int xdg_base(char *buf, size_t len, const char *var, const char *fallback)
{
    const char *value = getenv(var);
    const char *home;
    int n;

    if (value != NULL && value[0] == '/')
        return (size_t)snprintf(buf, len, "%s", value) < len ? 0 : -1;

    home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    n = snprintf(buf, len, "%s/%s", home, fallback);
    return (n > 0 && (size_t)n < len) ? 0 : -1;
}

int oma_path_config(char *buf, size_t len, const char *leaf)
{
    char base[512];

    if (xdg_base(base, sizeof(base), "XDG_CONFIG_HOME", ".config") != 0)
        return -1;
    return path_under(buf, len, base, leaf);
}

int oma_path_data(char *buf, size_t len, const char *leaf)
{
    char base[512];

    if (xdg_base(base, sizeof(base), "XDG_DATA_HOME", ".local/share") != 0)
        return -1;
    return path_under(buf, len, base, leaf);
}

int oma_path_state(char *buf, size_t len, const char *leaf)
{
    char base[512];

    if (xdg_base(base, sizeof(base), "XDG_STATE_HOME", ".local/state") != 0)
        return -1;
    return path_under(buf, len, base, leaf);
}

int oma_path_runtime(char *buf, size_t len, const char *leaf)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    char fallback[64];
    int n;

    /* The socket lives directly in the runtime dir, not in a subdirectory:
     * that directory is already per-user and wiped on logout. */
    if (dir == NULL || dir[0] != '/') {
        n = snprintf(fallback, sizeof(fallback), "/run/user/%u", (unsigned)getuid());
        if (n < 0 || (size_t)n >= sizeof(fallback)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        dir = fallback;
    }
    n = snprintf(buf, len, "%s/%s", dir, leaf);
    if (n < 0 || (size_t)n >= len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int oma_write_atomic(const char *path, const char *data, size_t len, mode_t mode)
{
    char tmp[512];
    int fd;
    ssize_t written;
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return -1;

    while (len > 0) {
        written = write(fd, data, len);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink(tmp);
            return -1;
        }
        data += written;
        len -= (size_t)written;
    }

    /* fsync before rename: without it a power cut can leave the rename applied
     * and the contents empty. */
    if (fsync(fd) != 0 || close(fd) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

char *oma_read_file(const char *path, size_t *out_len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    char *buf = NULL;
    size_t cap = 0, len = 0;

    if (fd < 0)
        return NULL;

    for (;;) {
        ssize_t got;

        if (len + 4096 + 1 > cap) {
            size_t next = cap == 0 ? 8192 : cap * 2;
            char *grown = realloc(buf, next);
            if (grown == NULL) {
                free(buf);
                close(fd);
                errno = ENOMEM;
                return NULL;
            }
            buf = grown;
            cap = next;
        }
        got = read(fd, buf + len, 4096);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return NULL;
        }
        if (got == 0)
            break;
        len += (size_t)got;
    }

    close(fd);
    buf[len] = '\0';
    if (out_len != NULL)
        *out_len = len;
    return buf;
}

void oma_id_new(char *buf, size_t len, const char *prefix)
{
    unsigned char rnd[4];
    int64_t now = oma_now();

    if (getrandom(rnd, sizeof(rnd), 0) != (ssize_t)sizeof(rnd)) {
        /* getrandom only fails here if the kernel is in a state where a weaker
         * id is the least of our problems; the timestamp still separates ids. */
        memset(rnd, 0, sizeof(rnd));
    }
    snprintf(buf, len, "%s%010llx%02x%02x%02x%02x", prefix,
             (unsigned long long)now, rnd[0], rnd[1], rnd[2], rnd[3]);
}
