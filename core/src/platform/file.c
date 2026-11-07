#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../internal.h"

uint64_t lv_clock_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static lv_file_version version(const struct stat *s)
{
    return (lv_file_version){(uint64_t)s->st_dev,         (uint64_t)s->st_ino,          (uint64_t)s->st_size,
                             (uint64_t)s->st_mtim.tv_sec, (uint64_t)s->st_mtim.tv_nsec, (uint64_t)s->st_ctim.tv_sec,
                             (uint64_t)s->st_ctim.tv_nsec};
}

FILE *lv_file_open(const char *path, lv_file_version *out, uint32_t *system_code)
{
    // O_NONBLOCK prevents FIFO open from hanging before the regular-file check.
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
    {
        *system_code = (uint32_t)errno;
        return nullptr;
    }
    struct stat s;
    if (fstat(fd, &s))
    {
        *system_code = (uint32_t)errno;
        close(fd);
        return nullptr;
    }
    if (!S_ISREG(s.st_mode) || s.st_size < 0)
    {
        *system_code = EINVAL;
        close(fd);
        return nullptr;
    }
    *out       = version(&s);
    FILE *file = fdopen(fd, "rb");
    if (!file)
    {
        *system_code = (uint32_t)errno;
        close(fd);
    }
    return file;
}

bool lv_file_unchanged(FILE *file, const char *path, const lv_file_version *original)
{
    struct stat opened, named;
    if (fstat(fileno(file), &opened) || stat(path, &named))
        return false;
    const lv_file_version a = version(&opened), b = version(&named);
    return !memcmp(&a, original, sizeof a) && !memcmp(&b, original, sizeof b);
}
void lv_file_close(FILE *file)
{
    if (file)
        fclose(file);
}
