#include "offline_archive.hpp"
#include "archive_projection.hpp"
#include "source_identity.hpp"

#include <algorithm>

namespace edgeecho::detail {
void require(bool condition, const std::string &message) {
    if (!condition) throw Error(message);
}

void validate_range(Range range) {
    require(range.start >= 0 && range.end > range.start && range.end <= max_time,
            "Expected a nonempty UTC microsecond range [start,end), years 1970..9999");
}

void throw_replay_error(const replay::Error &error) {
    if (error.code() == replay::ErrorCode::stale_snapshot)
        throw SourceChanged(error.what());
    throw Error(error.what());
}

ReadBudget::ReadBudget(Cancel cancel) : cancel_(std::move(cancel)) {}

void ReadBudget::reset(std::chrono::milliseconds timeout) {
    deadline_ = std::chrono::steady_clock::now() + timeout;
}

bool ReadBudget::expired() const noexcept {
    return (cancel_ && cancel_->load(std::memory_order_relaxed)) ||
           std::chrono::steady_clock::now() >= deadline_;
}

void ReadBudget::check() const {
    if (cancel_ && cancel_->load(std::memory_order_relaxed)) throw Error("Query cancelled");
    require(std::chrono::steady_clock::now() < deadline_,
            "Query time budget exceeded; narrow the time range");
}

std::chrono::milliseconds ReadBudget::remaining() const {
    check();
    return std::clamp(
        std::chrono::ceil<std::chrono::milliseconds>(deadline_ - std::chrono::steady_clock::now()),
        std::chrono::milliseconds(1), std::chrono::milliseconds(std::chrono::hours(24)));
}

namespace {
SourceIdentity file_identity(const std::filesystem::path &path) {
    const auto canonical = std::filesystem::canonical(path);
    return {canonical, std::filesystem::file_size(canonical),
            std::filesystem::last_write_time(canonical), native_source_token(canonical), {}, {}};
}

bool same_file(const SourceIdentity &left, const SourceIdentity &right) {
    return left.canonical_path == right.canonical_path && left.file_size == right.file_size &&
           left.modified == right.modified && left.native_token == right.native_token;
}

std::size_t record_bytes(const replay::Record &record) {
    auto size = 256 + record.id.size() + record.sensor_id.size() + record.data_type.size() +
        record.unit.size() + record.encoding.size() + record.source.size() + record.batch_id.size();
    if (const auto *block = std::get_if<replay::Bytes>(&record.value)) size += block->size();
    for (const auto &[key, value] : record.metadata)
        size += key.size() + value.size() + std::to_string(key.size()).size() +
                std::to_string(value.size()).size() + 2;
    return size;
}
} // namespace
} // namespace edgeecho::detail

namespace edgeecho {
namespace replay = detail::replay;
using detail::require;
using detail::validate_range;

Database::Impl::Impl(std::filesystem::path path, Limits l, Cancel cancel,
                     const std::optional<SourceIdentity> &expected)
    : limits(l), budget(std::move(cancel)) {
    require(l.page_records > 0 && l.page_records <= 100000 && l.page_bytes > 0 &&
            l.page_bytes <= 256 * 1024 * 1024 && l.record_bytes > 0 &&
            l.record_bytes <= 64 * 1024 * 1024 && l.scan_records > 0 && l.decode_samples > 0 &&
            l.series > 0 && l.series <= 100000 && l.query_timeout.count() > 0,
            "Invalid resource limits");
    budget.reset(limits.query_timeout);
    budget.check();
    if (std::filesystem::is_directory(path)) path /= "replay.db";
    if (!std::filesystem::is_regular_file(path)) {
        if (expected) throw SourceChanged("Source database is missing; reopen a closed offline file");
        throw Error("Database does not exist or is not a regular file");
    }
    source = detail::file_identity(path);
    if (expected && !detail::same_file(source, *expected))
        throw SourceChanged("Source database was replaced or modified; reopen it before continuing");
    const auto replay_page_bytes = std::max<std::size_t>(1024, limits.page_bytes);
    try {
        store_ = replay::Store::open_offline(
            path, {std::min(limits.record_bytes, replay_page_bytes), replay_page_bytes});
        snapshot_ = store_->snapshot();
    } catch (const replay::Error &error) {
        if (expected && error.code() == replay::ErrorCode::invalid_state)
            throw SourceChanged(error.what());
        detail::throw_replay_error(error);
    }
    if (!detail::same_file(source, detail::file_identity(path)))
        throw SourceChanged("Source database changed while opening; reopen it");
    source.database_id = snapshot_.database_id;
    source.generation = std::to_string(snapshot_.generation);
    if (expected && source != *expected)
        throw SourceChanged("Source database identity no longer matches the open session; reopen it");
}

Database::Impl::~Impl() = default;

void Database::Impl::check_source() const {
    const auto &cancel = budget.cancellation();
    if (cancel && cancel->load(std::memory_order_relaxed)) throw Error("Query cancelled");
    try {
        // Offline snapshots check the file and its sidecars before and after reading.
        const auto current = store_->snapshot();
        if (current.database_id != snapshot_.database_id || current.generation != snapshot_.generation ||
            current.records != snapshot_.records || current.events != snapshot_.events)
            throw SourceChanged("Source database changed; reopen an offline snapshot");
    } catch (const replay::Error &error) {
        detail::throw_replay_error(error);
    }
}

void Database::Impl::begin_operation() {
    budget.reset(limits.query_timeout);
    check_source();
}

detail::ArchiveProjection &Database::Impl::projection() {
    if (!projection_)
        projection_ = std::make_unique<detail::ArchiveProjection>(
            source.canonical_path, limits, budget, snapshot_.records);
    return *projection_;
}

Overview Database::Impl::overview() {
    return projection().overview();
}

Page Database::Impl::read_page(const Filter &filter, Range range, Cursor cursor, bool overlap) {
    return projection().page(filter, range, cursor, overlap);
}

void Database::Impl::scan(const Filter &filter, Range range,
                          const std::function<void(const replay::Record &)> &consume) {
    // The record table needs small pages for rendering. A window scan consumes
    // its pages internally and can amortize seeks without changing its byte cap.
    constexpr std::size_t history_page_records = 4096;
    const replay::SeriesQuery query{{range.start, range.end}, {filter.sensor, filter.type, filter.unit}};
    const auto page_size = std::min(history_page_records, limits.scan_records);
    std::optional<replay::Cursor> cursor;
    std::size_t rows = 0;
    try {
        do {
            auto page = store_->query_series(query, page_size, cursor, snapshot_,
                                            {budget.remaining(), budget.cancellation()});
            for (const auto &record : page.items) {
                budget.check();
                require(++rows <= limits.scan_records,
                        "Record scan budget exceeded; narrow the time range");
                require(detail::record_bytes(record) <= std::min(limits.record_bytes, limits.page_bytes),
                        "Record exceeds history byte budget; narrow the time range");
                consume(record);
            }
            cursor = std::move(page.next);
        } while (cursor);
    } catch (const replay::Error &error) {
        detail::throw_replay_error(error);
    }
}

std::vector<Event> Database::Impl::events(const Filter &filter, Range range, std::size_t limit) {
    validate_range(range);
    require(limit > 0 && limit <= 100000, "Invalid event limit");

    replay::EventSearchQuery query;
    query.range = {range.start, range.end};
    if (!filter.sensor.empty()) query.sensors = std::vector<std::string>{filter.sensor};
    if (!filter.type.empty()) query.types = std::vector<std::string>{filter.type};

    // Keep the public timeline order (start, sequence) for coincident events.
    std::vector<std::pair<std::int64_t, Event>> collected;
    std::optional<std::string> cursor;
    std::size_t bytes = 0;
    try {
        do {
            replay::EventSearchLimits search_limits;
            search_limits.timeout_ms = std::min<std::int64_t>(budget.remaining().count(), 3600000);
            auto page = store_->search_events(query, std::min<std::size_t>(limit + 1, 128),
                                              cursor, snapshot_, search_limits);
            for (auto &revision : page.items) {
                budget.check();
                require(collected.size() < limit, "Event count budget exceeded; narrow the time range");
                auto &event = revision.event;
                const auto size = 256 + event.id.size() + event.sensor_id.size() +
                    event.data_type.size() + event.event_type.size() + event.description.size();
                require(size <= limits.record_bytes && size <= limits.page_bytes - bytes,
                        "Event byte budget exceeded; narrow the time range");
                bytes += size;
                collected.emplace_back(revision.sequence,
                    Event{std::move(event.id), std::move(event.sensor_id), std::move(event.data_type),
                          std::move(event.event_type), std::move(event.description),
                          event.start, event.end, static_cast<int>(event.assessment),
                          static_cast<int>(event.severity), event.revision, revision.confirmed});
            }
            cursor = std::move(page.next);
        } while (cursor);
    } catch (const replay::Error &error) {
        detail::throw_replay_error(error);
    }
    std::sort(collected.begin(), collected.end(), [](const auto &a, const auto &b) {
        return a.second.start == b.second.start ? a.first < b.first : a.second.start < b.second.start;
    });
    std::vector<Event> result;
    result.reserve(collected.size());
    for (auto &item : collected) result.push_back(std::move(item.second));
    budget.check();
    return result;
}
} // namespace edgeecho
