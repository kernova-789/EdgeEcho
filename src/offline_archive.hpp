#pragma once

#include "archive_support.hpp"
#include <edgesense_replay/store.hpp>

namespace edgeecho::detail {
class ArchiveProjection;
} // namespace edgeecho::detail

namespace edgeecho {
struct Database::Impl {
    Impl(std::filesystem::path path, Limits limits, Cancel cancel,
         const std::optional<SourceIdentity> &expected);
    ~Impl();

    void begin_operation();
    void check_source() const;
    void check_budget() const { budget.check(); }
    Overview overview();
    Page read_page(const Filter &, Range, Cursor, bool overlap);
    std::vector<Event> events(const Filter &, Range, std::size_t limit);
    void scan(const Filter &, Range, const std::function<void(const detail::replay::Record &)> &);

    Limits limits;
    SourceIdentity source;
    detail::ReadBudget budget;

  private:
    detail::ArchiveProjection &projection();

    std::shared_ptr<detail::replay::Store> store_;
    detail::replay::Snapshot snapshot_;
    std::unique_ptr<detail::ArchiveProjection> projection_;
};
} // namespace edgeecho
