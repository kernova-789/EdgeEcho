#include "atomic_output.hpp"
#include "edgeecho/core.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <random>
#include <sstream>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace edgeecho::detail {
namespace {
constexpr std::size_t buffer_size = 64 * 1024;
std::string suffix() {
    static std::atomic<std::uint64_t> sequence{0};
    std::random_device random;
    std::ostringstream value;
    value << std::hex << random() << random() << sequence.fetch_add(1);
    return value.str();
}
#ifdef _WIN32
std::string native_error(const char *operation) {
    return std::string(operation) + " (Windows error " + std::to_string(GetLastError()) + ")";
}
#else
std::string native_error(const char *operation) {
    return std::string(operation) + ": " + std::strerror(errno);
}
#endif
} // namespace

struct AtomicOutput::Impl {
    std::filesystem::path destination, temporary;
    std::array<char, buffer_size> buffer{};
    std::size_t buffered = 0;
    std::uint64_t bytes = 0;
    bool published = false, external = false;
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int handle = -1;
#endif
    explicit Impl(std::filesystem::path target) : destination(std::move(target)) {
        std::error_code ec;
        const auto existing = std::filesystem::symlink_status(destination, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
            throw Error("Cannot inspect CSV destination: " + ec.message());
        if (std::filesystem::exists(existing))
            throw Error("Export target already exists; choose a new filename");
        const auto parent = destination.parent_path();
        if (!std::filesystem::is_directory(parent)) throw Error("CSV destination directory does not exist");
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            temporary = parent / std::filesystem::u8path(".edgeecho-export-" + suffix() + ".part");
#ifdef _WIN32
            handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (handle != INVALID_HANDLE_VALUE) return;
            const auto error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
                throw Error(native_error("Cannot create private export file"));
#else
            handle = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (handle >= 0) return;
            if (errno != EEXIST) throw Error(native_error("Cannot create private export file"));
#endif
        }
        temporary.clear();
        throw Error("Could not reserve a unique private export filename");
    }
    ~Impl() {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
#else
        if (handle >= 0) ::close(handle);
#endif
        if (!temporary.empty()) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            if (external) {
                for (const auto *tail : {"-wal", "-shm", "-journal"})
                    std::filesystem::remove(std::filesystem::u8path(temporary.u8string() + tail), ignored);
            }
        }
    }
    void native_write(const char *data, std::size_t count) {
        while (count) {
#ifdef _WIN32
            DWORD written = 0;
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(count, buffer_size));
            if (!WriteFile(handle, data, chunk, &written, nullptr) || written == 0)
                throw Error(native_error("CSV write failed"));
            const auto n = static_cast<std::size_t>(written);
#else
            const auto written = ::write(handle, data, count);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw Error(native_error("CSV write failed"));
            const auto n = static_cast<std::size_t>(written);
#endif
            data += n; count -= n;
        }
    }
    void flush() {
        if (!buffered) return;
        native_write(buffer.data(), buffered); buffered = 0;
    }
    void close_complete() {
        if (external) {
#ifdef _WIN32
            handle = CreateFileW(temporary.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) throw Error(native_error("Cannot reopen completed private output"));
#else
            handle = ::open(temporary.c_str(), O_RDWR | O_CLOEXEC);
            if (handle < 0) throw Error(native_error("Cannot reopen completed private output"));
#endif
        }
        flush();
#ifdef _WIN32
        if (!FlushFileBuffers(handle)) throw Error(native_error("CSV flush failed"));
        const auto old = handle; handle = INVALID_HANDLE_VALUE;
        if (!CloseHandle(old)) throw Error(native_error("CSV close failed"));
#else
        int rc;
        do { rc = ::fsync(handle); } while (rc < 0 && errno == EINTR);
        if (rc < 0) throw Error(native_error("CSV flush failed"));
        const auto old = handle; handle = -1;
        // Retrying close after EINTR can close an unrelated reused descriptor.
        if (::close(old) < 0) throw Error(native_error("CSV close failed"));
#endif
    }
};
AtomicOutput::AtomicOutput(std::filesystem::path destination)
    : impl_(std::make_unique<Impl>(std::move(destination))) {}
AtomicOutput::~AtomicOutput() = default;
void AtomicOutput::write(std::string_view data) {
    auto &i = *impl_;
    if (i.published) throw Error("Cannot append to a published export");
    if (i.external) throw Error("Private output is assigned to an external writer");
    if (data.size() > std::numeric_limits<std::uint64_t>::max() - i.bytes)
        throw Error("CSV byte count overflow");
    i.bytes += static_cast<std::uint64_t>(data.size());
    while (!data.empty()) {
        const auto amount = std::min(data.size(), buffer_size - i.buffered);
        std::memcpy(i.buffer.data() + i.buffered, data.data(), amount);
        i.buffered += amount; data.remove_prefix(amount);
        if (i.buffered == buffer_size) i.flush();
    }
}
const std::filesystem::path &AtomicOutput::prepare_external_write() {
    auto &i = *impl_;
    if (i.published || i.external || i.bytes != 0 || i.buffered != 0)
        throw Error("External writer requires a new empty private output");
#ifdef _WIN32
    const auto old = i.handle; i.handle = INVALID_HANDLE_VALUE;
    if (!CloseHandle(old)) throw Error(native_error("Cannot release private output handle"));
#else
    const auto old = i.handle; i.handle = -1;
    if (::close(old) < 0) throw Error(native_error("Cannot release private output handle"));
#endif
    i.external = true;
    return i.temporary;
}
void AtomicOutput::publish() {
    auto &i = *impl_;
    if (i.published) throw Error("Export is already published");
    i.close_complete();
#ifdef _WIN32
    // Without MOVEFILE_REPLACE_EXISTING the operation fails if another exporter
    // or user created the destination since our initial check.
    if (!MoveFileExW(i.temporary.c_str(), i.destination.c_str(), MOVEFILE_WRITE_THROUGH))
        throw Error(native_error("Cannot publish CSV without replacing an existing file"));
    i.published = true; i.temporary.clear();
#else
    // link() atomically installs the complete inode and fails for every existing
    // destination, including symlinks. Both names are on the same filesystem.
    if (::link(i.temporary.c_str(), i.destination.c_str()) < 0)
        throw Error(native_error("Cannot publish CSV without replacing an existing file"));
    i.published = true;
    if (::unlink(i.temporary.c_str()) == 0) i.temporary.clear();
    // Publication already succeeded: best-effort directory synchronization and
    // temporary-name cleanup cannot change that outcome into a false failure.
    const int directory = ::open(i.destination.parent_path().c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (directory >= 0) { (void)::fsync(directory); (void)::close(directory); }
#endif
}
std::uint64_t AtomicOutput::bytes_written() const noexcept { return impl_->bytes; }
const std::filesystem::path &AtomicOutput::temporary_path() const noexcept { return impl_->temporary; }
} // namespace edgeecho::detail
