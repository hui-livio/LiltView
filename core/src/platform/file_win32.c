#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "../internal.h"

static wchar_t *wide_path(const char *path, uint32_t *code)
{
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (!count)
    {
        *code = GetLastError();
        return NULL;
    }
    wchar_t *wide = malloc((size_t)count * sizeof *wide);
    if (!wide)
    {
        *code = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count))
    {
        *code = GetLastError();
        free(wide);
        return NULL;
    }
    return wide;
}

static bool file_version(HANDLE handle, lv_file_version *out)
{
    BY_HANDLE_FILE_INFORMATION identity;
    FILE_BASIC_INFO basic;
    if (GetFileType(handle) != FILE_TYPE_DISK)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }
    if (!GetFileInformationByHandle(handle, &identity) ||
        !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof basic))
        return false;
    if (identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        SetLastError(ERROR_DIRECTORY);
        return false;
    }
    const uint64_t modified = (uint64_t)basic.LastWriteTime.QuadPart;
    const uint64_t changed = (uint64_t)basic.ChangeTime.QuadPart;
    *out = (lv_file_version){
        identity.dwVolumeSerialNumber,
        ((uint64_t)identity.nFileIndexHigh << 32) | identity.nFileIndexLow,
        ((uint64_t)identity.nFileSizeHigh << 32) | identity.nFileSizeLow,
        modified / UINT64_C(10000000), (modified % UINT64_C(10000000)) * 100,
        changed / UINT64_C(10000000), (changed % UINT64_C(10000000)) * 100};
    return true;
}

uint64_t lv_clock_ns(void)
{
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    // Divide first to avoid overflowing after a long system uptime.
    return (uint64_t)(counter.QuadPart / frequency.QuadPart) * UINT64_C(1000000000) +
           (uint64_t)(counter.QuadPart % frequency.QuadPart) * UINT64_C(1000000000) / (uint64_t)frequency.QuadPart;
}

FILE *lv_file_open(const char *path, lv_file_version *out, uint32_t *system_code)
{
    wchar_t *wide = wide_path(path, system_code);
    if (!wide)
        return NULL;
    HANDLE handle = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    free(wide);
    if (handle == INVALID_HANDLE_VALUE)
    {
        *system_code = GetLastError();
        return NULL;
    }
    if (!file_version(handle, out))
    {
        *system_code = GetLastError();
        CloseHandle(handle);
        return NULL;
    }
    const int fd = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY | _O_NOINHERIT);
    if (fd == -1)
    {
        *system_code = ERROR_TOO_MANY_OPEN_FILES;
        CloseHandle(handle);
        return NULL;
    }
    FILE *file = _fdopen(fd, "rb");
    if (!file)
    {
        *system_code = ERROR_NOT_ENOUGH_MEMORY;
        _close(fd);
    }
    return file;
}

bool lv_file_unchanged(FILE *file, const char *path, const lv_file_version *original)
{
    lv_file_version opened, named;
    const intptr_t handle = _get_osfhandle(_fileno(file));
    if (handle == -1 || !file_version((HANDLE)handle, &opened))
        return false;
    uint32_t code = 0;
    wchar_t *wide = wide_path(path, &code);
    if (!wide)
        return false;
    // Reopen the pathname as well: replacement/rename must invalidate the source.
    HANDLE current = CreateFileW(wide, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wide);
    if (current == INVALID_HANDLE_VALUE)
        return false;
    const bool ok = file_version(current, &named);
    CloseHandle(current);
    return ok && !memcmp(&opened, original, sizeof opened) && !memcmp(&named, original, sizeof named);
}

void lv_file_close(FILE *file)
{
    if (file)
        fclose(file);
}
