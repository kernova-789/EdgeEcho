#include "edgeecho/core.hpp"
#include "source_identity.hpp"

#include <sstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#endif

namespace edgeecho {
bool operator==(const SourceIdentity &a, const SourceIdentity &b) {
    return a.canonical_path == b.canonical_path && a.file_size == b.file_size &&
           a.modified == b.modified && a.native_token == b.native_token &&
           a.database_id == b.database_id && a.generation == b.generation;
}
bool operator!=(const SourceIdentity &a, const SourceIdentity &b) { return !(a == b); }
namespace detail {
std::string native_source_token(const std::filesystem::path &path) {
    std::ostringstream token;
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw SourceChanged("Source database identity cannot be read; reopen a closed offline file");
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO basic{};
    const bool ok = GetFileInformationByHandle(handle, &info) &&
                   GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic));
    CloseHandle(handle);
    if (!ok) throw SourceChanged("Source filesystem does not expose a stable file identity");
    token << info.dwVolumeSerialNumber << ':' << info.nFileIndexHigh << ':' << info.nFileIndexLow
          << ':' << basic.CreationTime.QuadPart << ':' << basic.ChangeTime.QuadPart;
#else
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0)
        throw SourceChanged(std::string("Source database identity cannot be read: ") + std::strerror(errno));
    token << static_cast<std::uintmax_t>(info.st_dev) << ':' << static_cast<std::uintmax_t>(info.st_ino);
#if defined(__APPLE__)
    token << ':' << info.st_ctimespec.tv_sec << ':' << info.st_ctimespec.tv_nsec;
#else
    token << ':' << info.st_ctim.tv_sec << ':' << info.st_ctim.tv_nsec;
#endif
#endif
    return token.str();
}
} // namespace detail
} // namespace edgeecho
