#include "edgeecho/core.hpp"
#include "sqlite3.h"
#include "source_identity.hpp"
#include "atomic_output.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

namespace edgeecho {
namespace {
void require(bool condition, const std::string &message) {
    if (!condition) throw Error(message);
}
void validate_range(Range r) {
    require(r.start >= 0 && r.end > r.start && r.end <= max_time,
            "Expected a nonempty UTC microsecond range [start,end), years 1970..9999");
}
std::string uri_path(const std::filesystem::path &p) {
    auto bytes = std::filesystem::absolute(p).generic_u8string();
#ifdef _WIN32
    // SQLite URI drive paths are /C:/..., rather than relative C:/... paths.
    if (bytes.size() >= 2 && bytes[1] == ':') bytes.insert(bytes.begin(), '/');
#endif
    std::string out = "file:";
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char c : bytes) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '/' || c == ':' || c == '-' || c == '_' || c == '.')
            out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out + "?mode=ro&immutable=1";
}
struct Statement {
    sqlite3 *db;
    sqlite3_stmt *stmt = nullptr;
    Statement(sqlite3 *database, const std::string &sql) : db(database) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
            throw Error(std::string("SQLite prepare: ") + sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(stmt); }
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;
    void bind(int n, std::int64_t v) {
        if (sqlite3_bind_int64(stmt, n, v) != SQLITE_OK) throw Error("SQLite integer binding failed");
    }
    void bind(int n, const std::string &v) {
        require(v.size() <= 65536 && v.find('\0') == std::string::npos, "Invalid/oversized filter text");
        if (sqlite3_bind_text(stmt, n, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw Error("SQLite text binding failed");
    }
    bool step() {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw Error(std::string("SQLite query: ") + sqlite3_errmsg(db));
    }
    std::int64_t integer(int n) const { return sqlite3_column_int64(stmt, n); }
    double real(int n) const { return sqlite3_column_double(stmt, n); }
    bool null(int n) const { return sqlite3_column_type(stmt, n) == SQLITE_NULL; }
    std::string string(int n) const {
        auto *v = sqlite3_column_text(stmt, n);
        return v ? std::string(reinterpret_cast<const char *>(v), static_cast<std::size_t>(sqlite3_column_bytes(stmt, n))) : std::string{};
    }
};
std::int64_t one(sqlite3 *db, const std::string &sql) {
    Statement s(db, sql); require(s.step(), "Missing SQLite scalar result"); return s.integer(0);
}
void exec(sqlite3 *db, const char *sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw Error(std::string("SQLite: ") + sqlite3_errmsg(db));
}
std::string filters(const Filter &f) {
    std::string sql;
    if (!f.sensor.empty()) sql += " AND sensor=:sensor";
    if (!f.type.empty()) sql += " AND type=:type";
    if (!f.unit.empty()) sql += " AND unit=:unit";
    return sql;
}
void bind_filters(Statement &s, const Filter &f) {
    if (!f.sensor.empty()) s.bind(sqlite3_bind_parameter_index(s.stmt, ":sensor"), f.sensor);
    if (!f.type.empty()) s.bind(sqlite3_bind_parameter_index(s.stmt, ":type"), f.type);
    if (!f.unit.empty()) s.bind(sqlite3_bind_parameter_index(s.stmt, ":unit"), f.unit);
}
std::string csv(const std::string &s) {
    std::string result = "\"";
    for (char c : s) { result += c; if (c == '"') result += '"'; }
    return result + '"';
}
// Grouping buckets retain both extrema at their actual timestamps. A gap
// flushes the current segment so it cannot disappear during decimation.
class Envelope {
    Range range_;
    std::size_t buckets_, max_output_;
    std::optional<std::size_t> bucket_;
    Point min_, max_;
    std::vector<Point> result_;
    std::size_t *combined_bytes_ = nullptr, combined_limit_ = 0;
    void append(Point p) {
        require(result_.size() < max_output_, "Too many discontinuities; narrow the plot range");
        if (combined_bytes_) {
            require(sizeof(Point) <= combined_limit_ - *combined_bytes_, "Combined plot output byte budget exceeded; reduce viewport buckets or channels");
            *combined_bytes_ += sizeof(Point);
        }
        result_.push_back(p);
    }
    void flush() {
        if (!bucket_) return;
        if (min_.timestamp <= max_.timestamp) { append(min_); if (max_.timestamp != min_.timestamp || max_.value != min_.value) append(max_); }
        else { append(max_); append(min_); }
        bucket_.reset();
    }
  public:
    Envelope(Range r, std::size_t buckets, std::size_t *combined = nullptr, std::size_t limit = 0)
        : range_(r), buckets_(buckets), max_output_(buckets * 8 + 64), combined_bytes_(combined), combined_limit_(limit) {}
    void push(Point p) {
        if (p.timestamp < range_.start || p.timestamp >= range_.end) return;
        if (p.gap) {
            flush();
            if (result_.empty() || !result_.back().gap) append(p);
            return;
        }
        const auto index = static_cast<std::size_t>(static_cast<long double>(p.timestamp - range_.start) *
                                                   static_cast<long double>(buckets_) / (range_.end - range_.start));
        if (!bucket_ || *bucket_ != index) {
            flush(); bucket_ = index; min_ = max_ = p;
        } else {
            if (p.value < min_.value) min_ = p;
            if (p.value > max_.value) max_ = p;
        }
    }
    std::vector<Point> finish() { flush(); return std::move(result_); }
};
} // namespace

struct Database::Impl {
    sqlite3 *db = nullptr;
    std::filesystem::path input;
    Limits limits;
    Cancel cancel;
    std::uintmax_t file_size = 0;
    std::filesystem::file_time_type modified;
    std::string native_token;
    SourceIdentity source;
    std::chrono::steady_clock::time_point deadline;
    std::int64_t highwater = 0;
    Impl(std::filesystem::path path, Limits l, Cancel c, const std::optional<SourceIdentity> &expected)
        : limits(l), cancel(std::move(c)) {
        require(l.page_records > 0 && l.page_records <= 100000 && l.page_bytes > 0 && l.page_bytes <= 256 * 1024 * 1024 &&
                l.record_bytes > 0 && l.record_bytes <= 64 * 1024 * 1024 && l.scan_records > 0 &&
                l.decode_samples > 0 && l.series > 0 && l.series <= 100000 && l.query_timeout.count() > 0,
                "Invalid resource limits");
        if (std::filesystem::is_directory(path)) path /= "replay.db";
        if (!std::filesystem::is_regular_file(path)) {
            if (expected) throw SourceChanged("Source database is missing; reopen a closed offline file");
            throw Error("Database does not exist or is not a regular file");
        }
        input = std::filesystem::canonical(path);
        file_size = std::filesystem::file_size(input);
        modified = std::filesystem::last_write_time(input);
        native_token = detail::native_source_token(input);
        if (expected && (expected->canonical_path != input || expected->file_size != file_size ||
                         expected->modified != modified || expected->native_token != native_token))
            throw SourceChanged("Source database was replaced or modified; reopen it before continuing");
        check_source();
        const auto uri = uri_path(input);
        int rc = sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_PRIVATECACHE, nullptr);
        if (rc != SQLITE_OK) {
            std::string error = db ? sqlite3_errmsg(db) : "out of memory";
            if (db) sqlite3_close(db);
            db = nullptr;
            throw Error("Cannot open database: " + error);
        }
        try {
            sqlite3_limit(db, SQLITE_LIMIT_LENGTH, static_cast<int>(l.record_bytes + 1024 * 1024));
            sqlite3_busy_timeout(db, 1000);
            sqlite3_progress_handler(db, 1000, [](void *p) -> int {
                auto *self = static_cast<Impl *>(p);
                return (self->cancel && self->cancel->load()) || std::chrono::steady_clock::now() >= self->deadline;
            }, this);
            begin_operation();
            exec(db, "PRAGMA query_only=ON; PRAGMA trusted_schema=OFF; PRAGMA cache_size=-8192; BEGIN;");
            require(one(db, "PRAGMA application_id") == 1163088464, "Not an EdgeSense Replay database (application_id mismatch)");
            require(one(db, "PRAGMA user_version") == 3, "Only EdgeSense Replay schema v3 is supported; migrate a copy using the storage project");
            // Prepare required columns without decoding arbitrary data or migrating anything.
            Statement records(db, "SELECT seq,id,sensor,type,unit,ts,kind,scalar,block,quality,duration,rate,encoding,source,batch,metadata,version FROM records LIMIT 0");
            Statement history(db, "SELECT seq,id,sensor,type,start,end,assessment,severity,event_type,description,revision,revoked,confirmed FROM event_history LIMIT 0");
            require(one(db, "SELECT count(*) FROM sqlite_schema WHERE type='index' AND name IN ('records_series_time','records_time','event_history_identity','records_coverage_end')") == 4,
                    "Missing required Replay indexes");
            Statement meta(db, "SELECT value FROM meta WHERE key='database_id'");
            require(meta.step(), "Missing Replay database identity");
            const auto database_id = meta.string(0);
            require(database_id.size() == 32 && database_id.find_first_not_of("0123456789abcdef") == std::string::npos,
                    "Invalid Replay database identity");
            Statement generation_query(db, "SELECT value FROM meta WHERE key='generation'");
            require(generation_query.step(), "Missing Replay generation");
            const auto generation = generation_query.string(0);
            require(!generation.empty() && generation.size() <= 20 && generation.find_first_not_of("0123456789") == std::string::npos,
                    "Invalid Replay generation");
            source = {input, file_size, modified, native_token, database_id, generation};
            if (expected && source != *expected)
                throw SourceChanged("Source database identity no longer matches the open session; reopen it");
            highwater = one(db, "SELECT coalesce(max(seq),0) FROM records");
            check_source();
        } catch (...) { sqlite3_close(db); db = nullptr; throw; }
    }
    ~Impl() { if (db) sqlite3_close(db); }
    void check_source() {
        if (cancel && cancel->load()) throw Error("Query cancelled");
        try {
            std::error_code ec;
            auto wal = std::filesystem::u8path(input.u8string() + "-wal");
            const bool wal_exists = std::filesystem::exists(wal, ec);
            if (ec) throw SourceChanged("Cannot inspect source WAL; reopen the offline source");
            if (wal_exists) {
                const auto size = std::filesystem::file_size(wal, ec);
                if (ec) throw SourceChanged("Cannot inspect source WAL size; reopen the offline source");
                if (size != 0) throw SourceChanged("Nonempty WAL: close all storage writers to checkpoint, or open a standalone official backup; EdgeEcho only reads offline databases");
            }
            const auto journal = std::filesystem::u8path(input.u8string() + "-journal");
            const bool journal_exists = std::filesystem::exists(journal, ec);
            if (ec) throw SourceChanged("Cannot inspect source rollback journal; reopen the offline source");
            if (journal_exists) {
                const auto size = std::filesystem::file_size(journal, ec);
                if (ec) throw SourceChanged("Cannot inspect source rollback journal; reopen the offline source");
                if (size != 0) throw SourceChanged("Nonempty rollback journal: recover the database with the storage application, then open a closed standalone snapshot");
            }
            if (std::filesystem::file_size(input) != file_size || std::filesystem::last_write_time(input) != modified ||
                detail::native_source_token(input) != native_token)
                throw SourceChanged("Source database changed; stop the writer and reopen an offline snapshot");
        } catch (const std::filesystem::filesystem_error &) {
            throw SourceChanged("Source database can no longer be inspected; reopen a closed offline file");
        }
    }
    void begin_operation() { check_source(); deadline = std::chrono::steady_clock::now() + limits.query_timeout; }
    void check_budget() {
        if (cancel && cancel->load()) throw Error("Query cancelled");
        require(std::chrono::steady_clock::now() < deadline, "Query time budget exceeded; narrow the time range");
    }
    Page read_page(const Filter &f, Range r, Cursor after, int overlap) {
        validate_range(r);
        require(after.timestamp >= -1 && after.timestamp <= max_time && after.sequence >= 0, "Invalid record cursor");
        check_budget();
        // Plot pre-roll uses the v3 coverage index and then the ordinary time
        // index. An OR across all past history would make every late seek scan
        // records from the beginning of the series.
        const std::string covered = overlap == 2
            ? "ts<:start AND ts+duration>:start AND duration>0 AND quality=0 AND kind!=0 AND kind=2"
            : overlap == 1 ? "(ts>=:start OR (kind=2 AND duration>:start-ts))" : "1";
        // Size is returned before CASE materializes a potentially huge BLOB.
        const std::string weight = "(256+length(CAST(id AS BLOB))+length(CAST(sensor AS BLOB))+length(CAST(type AS BLOB))+length(CAST(unit AS BLOB))+coalesce(length(block),0)+length(CAST(encoding AS BLOB))+length(CAST(source AS BLOB))+length(CAST(batch AS BLOB))+length(CAST(metadata AS BLOB)))";
        Statement s(db, "SELECT seq,id,sensor,type,unit,ts,kind,scalar,CASE WHEN " + weight + "<=:budget THEN block ELSE NULL END,quality,duration,rate,encoding,source,batch,metadata,version," + weight +
                        (overlap == 2 ? " FROM records INDEXED BY records_coverage_end WHERE ts<:end AND " : " FROM records WHERE ts<:end AND ") + covered + filters(f) +
                        " AND seq<=:high AND ts>=:lower AND (ts,seq)>(:last_time,:last_seq) ORDER BY ts,seq LIMIT :count");
        auto bind = [&](const char *name, std::int64_t value) { s.bind(sqlite3_bind_parameter_index(s.stmt, name), value); };
        if (overlap != 0) bind(":start", r.start);
        // Give SQLite a single explicit scalar lower bound. Keeping only the
        // row-value predicate beside ts>=range.start can rescan earlier pages.
        bind(":lower", std::max(overlap == 0 ? r.start : Time{0}, after.timestamp));
        bind(":end", r.end); bind(":last_time", after.timestamp); bind(":last_seq", after.sequence);
        bind(":high", highwater); bind(":count", static_cast<std::int64_t>(limits.page_records + 1));
        bind(":budget", static_cast<std::int64_t>(limits.record_bytes)); bind_filters(s, f);
        Page page; page.next = after;
        std::size_t bytes = 0;
        while (s.step()) {
            check_budget();
            // Look-ahead does not reject an oversize next record in the current page.
            if (page.records.size() == limits.page_records) { page.more = true; break; }
            const auto size = s.integer(17);
            require(size >= 256 && static_cast<std::uint64_t>(size) <= limits.record_bytes, "Record exceeds record byte budget");
            if (static_cast<std::uint64_t>(size) > limits.page_bytes - bytes) {
                require(!page.records.empty(), "First record exceeds page byte budget");
                page.more = true; break;
            }
            Record row;
            row.sequence = s.integer(0); row.id = s.string(1); row.sensor = s.string(2); row.type = s.string(3); row.unit = s.string(4);
            require(s.integer(6) >= 0 && s.integer(6) <= 2, "Invalid stored record kind");
            row.timestamp = s.integer(5); row.kind = static_cast<int>(s.integer(6));
            row.quality = static_cast<std::uint32_t>(s.integer(9)); row.duration = s.integer(10);
            require(row.sequence > 0 && row.timestamp >= 0 && row.timestamp < max_time && row.duration >= 0 && row.duration <= max_time - row.timestamp &&
                    row.kind >= 0 && row.kind <= 2 && s.integer(9) >= 0 && s.integer(9) <= 15 && s.integer(16) == 1,
                    "Invalid stored record fields");
            if (row.kind == 1) {
                require(!s.null(7) && std::isfinite(s.real(7)), "Invalid scalar payload"); row.scalar = s.real(7);
            } else if (row.kind == 2) {
                const auto count = sqlite3_column_bytes(s.stmt, 8);
                const auto *ptr = static_cast<const std::uint8_t *>(sqlite3_column_blob(s.stmt, 8));
                require(count > 0 && ptr && row.duration > 0, "Invalid/empty block payload");
                row.block.assign(ptr, ptr + count);
            } else require(row.quality != 0, "Missing payload has good quality");
            if (!s.null(11)) { require(std::isfinite(s.real(11)) && s.real(11) > 0, "Invalid stored sample rate"); row.rate = s.real(11); }
            row.encoding = s.string(12); row.source = s.string(13); row.batch = s.string(14); row.metadata = s.string(15);
            require(row.kind != 2 || !row.encoding.empty(), "Block has no encoding");
            (void)unpack_metadata(row.metadata);
            page.next = {row.timestamp, row.sequence};
            bytes += static_cast<std::size_t>(size);
            page.records.push_back(std::move(row));
        }
        return page;
    }
    template<class Consumer> void scan(const Filter &f, Range r, Consumer consume) {
        std::size_t rows = 0;
        for (int mode : {2, 0}) {
            Cursor cursor;
            do {
                auto page = read_page(f, r, cursor, mode);
                for (const auto &row : page.records) {
                    check_budget();
                    require(++rows <= limits.scan_records, "Record scan budget exceeded; narrow the time range");
                    consume(row);
                }
                cursor = page.next;
                if (!page.more) break;
            } while (true);
        }
    }
};

Database::Database(std::filesystem::path path, Limits limits, Cancel cancel,
                   std::optional<SourceIdentity> expected_source) try
    : impl_(std::make_unique<Impl>(std::move(path), limits, std::move(cancel), expected_source)) {}
catch (const std::filesystem::filesystem_error &) {
    throw SourceChanged("Source path cannot be inspected; reopen a closed offline database");
}
Database::~Database() = default;
const std::filesystem::path &Database::path() const { return impl_->input; }
SourceIdentity Database::identity() const { impl_->check_source(); return impl_->source; }
Overview Database::overview() {
    auto &i = *impl_; i.begin_operation();
    Statement s(i.db, "SELECT sensor,type,unit,count(*),min(ts),max(ts+CASE WHEN duration>0 THEN duration ELSE 1 END),min(encoding),max(encoding),min(kind),max(kind) FROM records GROUP BY sensor,type,unit ORDER BY sensor,type,unit LIMIT ?");
    s.bind(1, static_cast<std::int64_t>(i.limits.series + 1));
    Overview result;
    while (s.step()) {
        require(result.series.size() < i.limits.series, "Too many series; database series budget exceeded");
        Series series; series.key = {s.string(0), s.string(1), s.string(2)};
        require(s.integer(3) > 0, "Invalid series count"); series.records = static_cast<std::uint64_t>(s.integer(3));
        series.range = {s.integer(4), s.integer(5)}; validate_range(series.range);
        series.encoding = s.string(6) == s.string(7) ? s.string(6) : "mixed";
        series.kind = s.integer(8) == s.integer(9) ? static_cast<int>(s.integer(8)) : -1;
        if (result.series.empty()) result.range = series.range;
        else { result.range.start = std::min(result.range.start, series.range.start); result.range.end = std::max(result.range.end, series.range.end); }
        result.records += series.records; result.series.push_back(std::move(series));
    }
    i.check_source(); return result;
}
Page Database::page(const Filter &f, Range range, Cursor cursor, bool overlap) {
    auto &i = *impl_; i.begin_operation(); auto result = i.read_page(f, range, cursor, overlap); i.check_source(); return result;
}
std::vector<Event> Database::events(const Filter &f, Range r, std::size_t limit) {
    auto &i = *impl_; i.begin_operation(); validate_range(r);
    require(limit > 0 && limit <= 100000, "Invalid event limit");
    std::string filter;
    if (!f.sensor.empty()) filter += " AND e.sensor=:sensor";
    if (!f.type.empty()) filter += " AND (e.type=:type OR e.type='')";
    Statement s(i.db, "SELECT e.id,e.sensor,e.type,e.start,e.end,e.assessment,e.severity,e.event_type,e.description,e.revision,e.confirmed FROM event_history e WHERE e.revoked=0 AND NOT EXISTS(SELECT 1 FROM event_history n WHERE n.id=e.id AND n.seq>e.seq) AND e.start<:end AND e.end>:start" + filter + " ORDER BY e.start,e.seq LIMIT :count");
    auto bind = [&](const char *name, Time value) { s.bind(sqlite3_bind_parameter_index(s.stmt, name), value); };
    bind(":start", r.start); bind(":end", r.end); bind(":count", static_cast<std::int64_t>(limit + 1));
    if (!f.sensor.empty()) s.bind(sqlite3_bind_parameter_index(s.stmt, ":sensor"), f.sensor);
    if (!f.type.empty()) s.bind(sqlite3_bind_parameter_index(s.stmt, ":type"), f.type);
    std::vector<Event> result;
    std::size_t bytes = 0;
    while (s.step()) {
        i.check_budget();
        require(result.size() < limit, "Event count budget exceeded; narrow the time range");
        std::size_t size = 256;
        for (int column : {0, 1, 2, 7, 8}) size += static_cast<std::size_t>(sqlite3_column_bytes(s.stmt, column));
        require(size <= i.limits.record_bytes && size <= i.limits.page_bytes - bytes,
                "Event byte budget exceeded; narrow the time range");
        bytes += size;
        Event event;
        event.id = s.string(0); event.sensor = s.string(1); event.type = s.string(2); event.start = s.integer(3); event.end = s.integer(4);
        validate_range({event.start, event.end});
        require(s.integer(5) >= 0 && s.integer(5) <= 4 && s.integer(6) >= 0 && s.integer(6) <= 2, "Invalid event assessment/severity");
        event.assessment = static_cast<int>(s.integer(5)); event.severity = static_cast<int>(s.integer(6));
        event.event_type = s.string(7); event.description = s.string(8); event.revision = s.integer(9); event.confirmed = s.integer(10) != 0;
        require(event.revision > 0, "Invalid event revision"); result.push_back(std::move(event));
    }
    i.check_source(); return result;
}
Plot Database::plot(const Filter &f, Range r, const std::string &channel, std::size_t buckets) {
    auto result = plots(f, r, {channel}, buckets);
    return std::move(result.front());
}
std::vector<Plot> Database::plots(const Filter &f, Range r, const std::vector<std::string> &selected,
                                 std::size_t buckets) {
    auto &i = *impl_; i.begin_operation(); validate_range(r);
    require(!f.sensor.empty() && !f.type.empty() && !f.unit.empty(), "Plot requires an exact sensor/type/unit series");
    require(buckets > 0 && buckets <= 100000, "Invalid plot bucket count");
    require(!selected.empty() && selected.size() <= 16, "Select between one and sixteen channels");
    std::set<std::string> unique;
    for (const auto &channel : selected)
        require(!channel.empty() && channel.size() <= 512 && unique.insert(channel).second,
                "Channel IDs must be nonempty and distinct");
    std::vector<Envelope> envelopes;
    std::size_t combined_output_bytes = 0;
    for (std::size_t n = 0; n < selected.size(); ++n) envelopes.emplace_back(r, buckets, &combined_output_bytes, i.limits.page_bytes);
    std::vector<Plot> results(selected.size());
    std::size_t decoded = 0;
    std::optional<Time> previous_end;
    i.scan(f, r, [&](const Record &row) {
        const bool waveform = row.kind == 2 && row.encoding != "iio-le-72-v1";
        for (std::size_t n = 0; n < selected.size(); ++n) {
            auto &result = results[n]; auto &envelope = envelopes[n];
            ++result.records_scanned;
            if (row.quality != 0 || row.kind == 0) {
                ++result.quality_skipped; envelope.push({std::max(r.start, row.timestamp), 0, true}); continue;
            }
            if (waveform && previous_end && row.timestamp > *previous_end + 1)
                envelope.push({std::max(r.start, *previous_end), 0, true});
            if (waveform && previous_end && row.timestamp < *previous_end)
                envelope.push({std::max(r.start, row.timestamp), 0, true});
            decode(row, selected[n], [&](Point point) {
                require(++decoded <= i.limits.decode_samples, "Plot decoded value budget exceeded; narrow the time range");
                ++result.samples_decoded;
                if ((decoded & 4095U) == 0) i.check_budget();
                envelope.push(point);
            }, i.cancel);
            if (waveform) result.estimated_time = true;
        }
        if (waveform && row.quality == 0) previous_end = row.timestamp + row.duration;
        else previous_end.reset();
    });
    std::size_t result_bytes = 0;
    for (std::size_t n = 0; n < results.size(); ++n) {
        results[n].points = envelopes[n].finish();
        const auto bytes = results[n].points.size() * sizeof(Point);
        require(bytes <= i.limits.page_bytes - result_bytes, "Combined plot output byte budget exceeded; reduce viewport buckets or channels");
        result_bytes += bytes;
    }
    i.check_source(); return results;
}
std::optional<Point> Database::probe(const Filter &f, Range r, const std::string &channel, Time target) {
    auto &i = *impl_; i.begin_operation(); validate_range(r);
    require(!f.sensor.empty() && !f.type.empty() && !f.unit.empty(), "Probe requires an exact sensor/type/unit series");
    require(target >= r.start && target < r.end, "Probe target is outside the visible half-open range");
    std::optional<Point> closest;
    Time distance = max_time;
    std::size_t decoded = 0;
    i.scan(f, r, [&](const Record &row) {
        if (row.kind == 0 || row.quality != 0) return;
        decode(row, channel, [&](Point point) {
            require(++decoded <= i.limits.decode_samples, "Probe decoded sample budget exceeded; narrow the time range");
            if ((decoded & 4095U) == 0) i.check_budget();
            if (point.gap || point.timestamp < r.start || point.timestamp >= r.end) return;
            const auto delta = point.timestamp >= target ? point.timestamp - target : target - point.timestamp;
            // Earlier timestamps win ties. Same-time rows retain (ts,seq) order.
            if (!closest || delta < distance || (delta == distance && point.timestamp < closest->timestamp)) {
                closest = point; distance = delta;
            }
        }, i.cancel);
    });
    i.check_source(); return closest;
}
AudioClip Database::audio(const Filter &f, Range r, const std::string &channel,
                          AudioOptions options) {
    auto &i = *impl_; i.begin_operation(); validate_range(r);
    require(!f.sensor.empty() && !f.type.empty() && !f.unit.empty(), "Audio synthesis requires an exact sensor/type/unit series");
    require(!channel.empty() && channel.size() <= 512, "Audio requires a nonempty channel ID");
    require(std::isfinite(options.gain) && options.gain >= 0.0 && options.gain <= 1000.0, "Audio gain must be in [0, 1000]");
    require(options.sample_rate_hz == 0 || (options.sample_rate_hz >= 100 && options.sample_rate_hz <= 192000),
            "Audio target sample rate must be 0 (auto) or in [100, 192000] Hz");
    require(options.timeout.count() > 0 && options.timeout <= std::chrono::hours(24), "Invalid audio synthesis timeout");
    i.deadline = std::chrono::steady_clock::now() + options.timeout;

    struct RawSample { double value = 0.0; bool is_gap = false; };
    std::vector<RawSample> raw;
    double detected_rate = 0.0;
    AudioClip clip;
    clip.range = r;
    std::size_t total_emitted = 0;
    std::optional<Time> previous_end;

    i.scan(f, r, [&](const Record &row) {
        ++clip.records_scanned;
        if (row.rate && *row.rate > 0.0 && detected_rate <= 0.0)
            detected_rate = *row.rate;
        const double rate_for_gap = detected_rate > 0.0 ? detected_rate : 1000.0;
        const Time clip_start = std::max(r.start, row.timestamp);
        const Time clip_end = std::min(r.end, row.timestamp + std::max<Time>(1, row.duration));
        if (previous_end && clip_start > *previous_end + 1) {
            const auto gap_us = clip_start - *previous_end;
            const auto gap_count = static_cast<std::size_t>(std::clamp<double>(
                std::round(static_cast<double>(gap_us) * rate_for_gap / 1000000.0), 0.0, 480000.0));
            for (std::size_t g = 0; g < gap_count; ++g) {
                require(++total_emitted <= i.limits.decode_samples, "Audio sample budget exceeded; narrow the time range");
                raw.push_back({0.0, true});
                ++clip.gap_samples;
            }
        }
        if (row.quality != 0 || row.kind == 0) {
            const auto span_us = std::max<Time>(1, clip_end - clip_start);
            const auto gap_count = static_cast<std::size_t>(std::clamp<double>(
                std::round(static_cast<double>(span_us) * rate_for_gap / 1000000.0), 1.0, 480000.0));
            for (std::size_t g = 0; g < gap_count; ++g) {
                require(++total_emitted <= i.limits.decode_samples, "Audio sample budget exceeded; narrow the time range");
                raw.push_back({0.0, true});
                ++clip.gap_samples;
            }
            previous_end = clip_end;
            return;
        }
        decode(row, channel, [&](Point point) {
            if (point.timestamp < r.start || point.timestamp >= r.end) return;
            require(++total_emitted <= i.limits.decode_samples, "Audio sample budget exceeded; narrow the time range");
            if ((total_emitted & 4095U) == 0) i.check_budget();
            if (point.gap || !std::isfinite(point.value)) {
                raw.push_back({0.0, true});
                ++clip.gap_samples;
            } else {
                raw.push_back({point.value, false});
                ++clip.samples_decoded;
            }
        }, i.cancel);
        previous_end = clip_end;
    });

    require(clip.samples_decoded > 0 && !raw.empty(), "No valid audio samples in the selected range");
    if (detected_rate <= 0.0) {
        const double duration_s = static_cast<double>(r.end - r.start) / 1000000.0;
        detected_rate = duration_s > 0.0 ? static_cast<double>(raw.size()) / duration_s : 1000.0;
    }
    const auto nominal_rate = static_cast<std::uint32_t>(
        std::clamp(std::lround(detected_rate), 100L, 192000L));
    double mean = 0.0;
    if (options.remove_dc) {
        double sum = 0.0;
        for (const auto &s : raw) if (!s.is_gap) sum += s.value;
        mean = sum / static_cast<double>(clip.samples_decoded);
    }
    double peak = 0.0;
    for (const auto &s : raw) {
        if (!s.is_gap) peak = std::max(peak, std::abs(s.value - mean));
    }
    clip.peak_raw = peak;
    double scale = options.gain;
    if (options.normalize && peak > 0.0) {
        scale = (29490.0 / peak) * options.gain;
    } else if (!options.normalize && peak > 32767.0) {
        scale = (32767.0 / 2147483648.0) * options.gain;
    }
    std::vector<std::int16_t> pcm(raw.size(), 0);
    for (std::size_t idx = 0; idx < raw.size(); ++idx) {
        if (raw[idx].is_gap) {
            pcm[idx] = 0;
        } else {
            const double v = (raw[idx].value - mean) * scale;
            pcm[idx] = static_cast<std::int16_t>(std::clamp(std::lround(v), -32768L, 32767L));
        }
    }
    const std::uint32_t out_rate = options.sample_rate_hz != 0 ? options.sample_rate_hz : nominal_rate;
    if (out_rate != nominal_rate && pcm.size() > 1) {
        const auto out_count = static_cast<std::size_t>(std::clamp<std::uint64_t>(
            (static_cast<std::uint64_t>(pcm.size()) * out_rate + nominal_rate / 2U) / nominal_rate,
            1ULL, static_cast<std::uint64_t>(i.limits.decode_samples)));
        std::vector<std::int16_t> resampled(out_count, 0);
        const double ratio = static_cast<double>(nominal_rate) / static_cast<double>(out_rate);
        for (std::size_t idx = 0; idx < out_count; ++idx) {
            const double pos = static_cast<double>(idx) * ratio;
            const auto base = static_cast<std::size_t>(pos);
            if (base + 1 >= pcm.size()) resampled[idx] = pcm.back();
            else {
                const double frac = pos - static_cast<double>(base);
                const double interp = static_cast<double>(pcm[base]) * (1.0 - frac) +
                                      static_cast<double>(pcm[base + 1]) * frac;
                resampled[idx] = static_cast<std::int16_t>(std::clamp(std::lround(interp), -32768L, 32767L));
            }
        }
        clip.pcm16 = std::move(resampled);
    } else {
        clip.pcm16 = std::move(pcm);
    }
    clip.sample_rate_hz = out_rate;
    i.check_source();
    return clip;
}
std::uint64_t Database::export_wav(const std::filesystem::path &target, const Filter &f, Range r,
                                   const std::string &channel, AudioOptions options) {
    auto &i = *impl_;
    const auto destination = std::filesystem::weakly_canonical(std::filesystem::absolute(target));
    require(destination != i.input && destination.u8string() != i.input.u8string() + "-wal" &&
            destination.u8string() != i.input.u8string() + "-shm" &&
            destination.u8string() != i.input.u8string() + "-journal",
            "Cannot overwrite source database or sidecars");
    const auto clip = audio(f, r, channel, options);
    const auto wav_bytes = encode_wav(clip);
    detail::AtomicOutput output(destination);
    output.write(std::string_view(reinterpret_cast<const char *>(wav_bytes.data()), wav_bytes.size()));
    i.check_source();
    output.publish();
    return clip.pcm16.size();
}
Spectrum Database::spectrum(const Filter &f, Range r, const std::string &channel, std::size_t max_bins) {
    auto &i = *impl_; i.begin_operation(); validate_range(r);
    require(!f.sensor.empty() && !f.type.empty() && !f.unit.empty(), "Spectrum requires an exact sensor/type/unit series");
    require(!channel.empty() && channel.size() <= 512, "Spectrum requires a nonempty channel ID");
    require(max_bins >= 4 && max_bins <= 65536, "Spectrum max_bins must be in [4, 65536]");
    std::vector<double> samples;
    samples.reserve(16384);
    std::vector<Time> timestamps;
    timestamps.reserve(1024);
    double detected_rate = 0.0;
    std::size_t decoded = 0;
    i.scan(f, r, [&](const Record &row) {
        if (row.kind == 0 || row.quality != 0) return;
        if (row.rate && *row.rate > 0.0 && detected_rate <= 0.0)
            detected_rate = *row.rate;
        decode(row, channel, [&](Point point) {
            require(++decoded <= i.limits.decode_samples, "Spectrum decoded sample budget exceeded; narrow the time range");
            if ((decoded & 4095U) == 0) i.check_budget();
            if (point.gap || !std::isfinite(point.value) || point.timestamp < r.start || point.timestamp >= r.end)
                return;
            if (samples.size() < 65536) {
                samples.push_back(point.value);
                if (timestamps.size() < 1024)
                    timestamps.push_back(point.timestamp);
            }
        }, i.cancel);
    });
    require(samples.size() >= 8, "Spectrum requires at least 8 valid samples in the selected range");
    if (detected_rate <= 0.0 && timestamps.size() >= 2) {
        const Time span = timestamps.back() - timestamps.front();
        if (span > 0)
            detected_rate = static_cast<double>(timestamps.size() - 1) * 1000000.0 / static_cast<double>(span);
    }
    if (detected_rate <= 0.0) detected_rate = 1.0;
    auto result = compute_spectrum(samples, detected_rate, max_bins);
    i.check_source();
    return result;
}
std::uint64_t Database::export_csv(const std::filesystem::path &target, const Filter &f, Range r,
                                   ExportOptions options) {
    auto &i = *impl_; i.begin_operation(); validate_range(r);
    require(options.timeout.count() > 0 && options.timeout <= std::chrono::hours(24 * 7),
            "Export timeout must be positive and no longer than seven days");
    const auto destination = std::filesystem::weakly_canonical(std::filesystem::absolute(target));
    require(destination != i.input && destination.u8string() != i.input.u8string() + "-wal" && destination.u8string() != i.input.u8string() + "-shm" && destination.u8string() != i.input.u8string() + "-journal", "Cannot overwrite source database or sidecars");
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    detail::AtomicOutput output(destination);
    ExportProgress progress;
    auto check_export = [&] {
        if (i.cancel && i.cancel->load()) throw Error("Export cancelled");
        require(std::chrono::steady_clock::now() < deadline, "Export time budget exceeded");
    };
    auto report = [&] {
        check_export(); progress.bytes_written = output.bytes_written();
        if (options.progress) options.progress(progress);
        check_export();
    };
    auto write = [&](std::string_view text) { check_export(); output.write(text); };
    write("id,sensor,type,unit,timestamp_us,kind,scalar,block_hex,quality,duration_us,sample_rate_hz,encoding,source,batch,metadata_packed\r\n");
    report();
    Cursor cursor;
    do {
        // Query work remains bounded per page, while export has its own longer
        // total deadline. Waiting in a progress observer is not query work.
        check_export(); i.begin_operation();
        auto page = i.read_page(f, r, cursor, false);
        for (const auto &row : page.records) {
            std::ostringstream prefix;
            prefix.imbue(std::locale::classic()); prefix << std::setprecision(17);
            prefix << csv(row.id) << ',' << csv(row.sensor) << ',' << csv(row.type) << ',' << csv(row.unit) << ',' << row.timestamp << ',' << row.kind << ',';
            if (row.scalar) prefix << *row.scalar;
            prefix << ','; write(prefix.str());
            constexpr char digits[] = "0123456789abcdef";
            std::string hex_chunk; hex_chunk.reserve(16 * 1024);
            for (std::size_t offset = 0; offset < row.block.size();) {
                const auto amount = std::min<std::size_t>(8192, row.block.size() - offset);
                hex_chunk.clear();
                for (std::size_t n = 0; n < amount; ++n) {
                    const auto value = row.block[offset + n];
                    hex_chunk += digits[value >> 4]; hex_chunk += digits[value & 15];
                }
                write(hex_chunk); offset += amount; report();
            }
            std::ostringstream suffix;
            suffix.imbue(std::locale::classic()); suffix << std::setprecision(17);
            suffix << ',' << row.quality << ',' << row.duration << ',';
            if (row.rate) suffix << *row.rate;
            suffix << ',' << csv(row.encoding) << ',' << csv(row.source) << ',' << csv(row.batch) << ',' << csv(row.metadata) << "\r\n";
            write(suffix.str()); ++progress.records_written; report();
        }
        cursor = page.next; if (!page.more) break;
    } while (true);
    check_export(); i.check_source();
    output.publish();
    progress.bytes_written = output.bytes_written(); progress.published = true;
    // Once the complete file is installed, observers cannot revoke publication
    // or turn success into an exception that encourages an unsafe retry.
    if (options.progress) { try { options.progress(progress); } catch (...) {} }
    return progress.records_written;
}
} // namespace edgeecho
