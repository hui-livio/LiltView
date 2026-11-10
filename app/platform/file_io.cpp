#include "file_io.hpp"
#include <algorithm>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#include <atomic>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace liltview::platform
{
namespace
{
[[noreturn]] void io_error(const char *message)
{
    throw std::system_error(errno ? errno : EIO, std::generic_category(), message);
}
#ifdef _WIN32
[[noreturn]] void windows_error(const char *message)
{
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), message);
}
#endif
}
void seek_file(std::FILE *file, std::uint64_t offset)
{
#ifdef _WIN32
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<__int64>::max()) ||
        _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) != 0)
#else
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0)
#endif
        io_error("Cannot seek native tile cache");
}
std::FILE *temporary_file(const std::filesystem::path &directory)
{
#ifdef _WIN32
    static std::atomic<unsigned long long> sequence{0};
    HANDLE handle = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 100; ++attempt)
    {
        const auto name = directory / (L"native-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence++));
        handle = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (handle != INVALID_HANDLE_VALUE)
            break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
            windows_error("Cannot create native tile cache");
    }
    if (handle == INVALID_HANDLE_VALUE)
        windows_error("Cannot create unique native tile cache");
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDWR | _O_BINARY | _O_NOINHERIT);
    if (fd < 0)
    {
        CloseHandle(handle);
        io_error("Cannot open native tile cache descriptor");
    }
    auto *file = _fdopen(fd, "w+b");
#else
    auto name = (directory / "native-XXXXXX").string();
    const int fd = mkstemp(name.data());
    if (fd < 0)
        io_error("Cannot create native tile cache");
    if (unlink(name.c_str()) != 0)
    {
        close(fd);
        io_error("Cannot unlink native tile cache");
    }
    auto *file = fdopen(fd, "w+b");
#endif
    if (!file)
    {
        close_descriptor(fd);
        io_error("Cannot open native tile cache");
    }
    return file;
}
int file_descriptor(std::FILE *file)
{
#ifdef _WIN32
    return _fileno(file);
#else
    return fileno(file);
#endif
}
int duplicate_descriptor(int fd)
{
    if (fd < 0)
        throw std::system_error(EBADF, std::generic_category(), "Invalid native tile cache descriptor");
#ifdef _WIN32
    // A separate overlapped handle preserves pread semantics while the UI may
    // append tiles: neither reader nor writer changes the other's file offset.
    const HANDLE handle = ReOpenFile(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_FLAG_OVERLAPPED);
    if (handle == INVALID_HANDLE_VALUE)
        windows_error("Cannot reopen native tile cache");
    const int copy = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY | _O_NOINHERIT);
    if (copy < 0)
        CloseHandle(handle);
#else
    const int copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
#endif
    if (copy < 0)
        io_error("Cannot duplicate native tile cache descriptor");
    return copy;
}
void close_descriptor(int fd) noexcept
{
#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
}
std::size_t read_at(int fd, void *data, std::size_t bytes, std::uint64_t offset)
{
#ifdef _WIN32
    const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    OVERLAPPED position{};
    position.Offset = static_cast<DWORD>(offset);
    position.OffsetHigh = static_cast<DWORD>(offset >> 32);
    position.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!position.hEvent)
        windows_error("Cannot create native tile cache read event");
    DWORD count = 0;
    const BOOL ok = ReadFile(handle, data, static_cast<DWORD>(std::min<std::size_t>(bytes, MAXDWORD)), &count, &position);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && error == ERROR_IO_PENDING)
        error = GetOverlappedResult(handle, &position, &count, TRUE) ? ERROR_SUCCESS : GetLastError();
    CloseHandle(position.hEvent);
    if (error == ERROR_HANDLE_EOF)
        return 0;
    if (error != ERROR_SUCCESS)
        throw std::system_error(static_cast<int>(error), std::system_category(), "Cannot read cached native tile");
    return count;
#else
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
        throw std::invalid_argument("Cached tile file offset overflow");
    ssize_t count;
    do
    {
        count = pread(fd, data, bytes, static_cast<off_t>(offset));
    } while (count < 0 && errno == EINTR);
    if (count < 0)
        io_error("Cannot read cached native tile");
    return static_cast<std::size_t>(count);
#endif
}
} // namespace liltview::platform
