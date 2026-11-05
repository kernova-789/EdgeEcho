#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace edgeecho::detail {
// Owns a private, exclusively created file beside the destination. Publishing
// never replaces another directory entry. Unpublished temporary data is removed
// by the destructor; the destination is never removed by cleanup.
class AtomicOutput {
  public:
    explicit AtomicOutput(std::filesystem::path destination);
    ~AtomicOutput();
    AtomicOutput(const AtomicOutput &) = delete;
    AtomicOutput &operator=(const AtomicOutput &) = delete;
    void write(std::string_view);
    // For a writer such as SQLite that must open its own path. Releases our
    // empty-file handle; publish reopens and synchronizes it after that writer
    // has closed. The reserved private filename remains exclusively ours.
    const std::filesystem::path &prepare_external_write();
    void publish();
    std::uint64_t bytes_written() const noexcept;
    const std::filesystem::path &temporary_path() const noexcept;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace edgeecho::detail
