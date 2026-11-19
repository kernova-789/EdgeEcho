#pragma once

#include "archive_support.hpp"

struct sqlite3;

namespace edgeecho::detail {
// The v3 fields needed by archive inspection and lossless CSV export.
class ArchiveProjection {
  public:
    ArchiveProjection(const std::filesystem::path &, const Limits &, ReadBudget &,
                      std::int64_t highwater);
    ~ArchiveProjection();
    ArchiveProjection(const ArchiveProjection &) = delete;
    ArchiveProjection &operator=(const ArchiveProjection &) = delete;

    Overview overview();
    Page page(const Filter &, Range, Cursor, bool overlap);

  private:
    sqlite3 *db_ = nullptr;
    const Limits &limits_;
    const ReadBudget &budget_;
    std::int64_t highwater_;
};
} // namespace edgeecho::detail
