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
    void publish();
    std::uint64_t bytes_written() const noexcept;
    const std::filesystem::path &temporary_path() const noexcept;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace edgeecho::detail
