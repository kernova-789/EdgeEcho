#include "archive_projection.hpp"
#include "sqlite3.h"

#include <algorithm>
#include <cmath>

namespace edgeecho::detail {
namespace {
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
} // namespace

ArchiveProjection::ArchiveProjection(const std::filesystem::path &path, const Limits &limits,
                                     ReadBudget &budget, std::int64_t highwater)
    : limits_(limits), budget_(budget), highwater_(highwater) {
    const auto uri = uri_path(path);
    const int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_PRIVATECACHE;
    const auto rc = sqlite3_open_v2(uri.c_str(), &db_, flags, nullptr);
    try {
        if (rc != SQLITE_OK)
            throw Error(std::string("Cannot open archive projection: ") +
                        (db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(rc)));
        sqlite3_limit(db_, SQLITE_LIMIT_LENGTH, static_cast<int>(limits_.record_bytes + 1024 * 1024));
        sqlite3_progress_handler(db_, 1000, [](void *context) -> int {
            return static_cast<const ReadBudget *>(context)->expired();
        }, &budget);
        exec(db_, "PRAGMA query_only=ON; PRAGMA trusted_schema=OFF; PRAGMA cache_size=-8192;");
    } catch (...) {
        sqlite3_close(db_);
        throw;
    }
}

ArchiveProjection::~ArchiveProjection() {
    sqlite3_close(db_);
}

Overview ArchiveProjection::overview() {
    Statement s(db_, "SELECT sensor,type,unit,count(*),min(ts),max(ts+CASE WHEN duration>0 THEN duration ELSE 1 END),min(encoding),max(encoding),min(kind),max(kind) FROM records GROUP BY sensor,type,unit ORDER BY sensor,type,unit LIMIT ?");
    s.bind(1, static_cast<std::int64_t>(limits_.series + 1));
    Overview result;
    while (s.step()) {
        require(result.series.size() < limits_.series, "Too many series; database series budget exceeded");
        Series series; series.key = {s.string(0), s.string(1), s.string(2)};
        require(s.integer(3) > 0, "Invalid series count"); series.records = static_cast<std::uint64_t>(s.integer(3));
        series.range = {s.integer(4), s.integer(5)}; validate_range(series.range);
        series.encoding = s.string(6) == s.string(7) ? s.string(6) : "mixed";
        series.kind = s.integer(8) == s.integer(9) ? static_cast<int>(s.integer(8)) : -1;
        if (result.series.empty()) result.range = series.range;
        else { result.range.start = std::min(result.range.start, series.range.start); result.range.end = std::max(result.range.end, series.range.end); }
        result.records += series.records; result.series.push_back(std::move(series));
    }
    return result;
}

Page ArchiveProjection::page(const Filter &f, Range r, Cursor after, bool overlap) {
    validate_range(r);
    require(after.timestamp >= -1 && after.timestamp <= max_time && after.sequence >= 0, "Invalid record cursor");
    budget_.check();
    // Size is returned before CASE materializes a potentially huge BLOB.
    const std::string weight = "(256+length(CAST(id AS BLOB))+length(CAST(sensor AS BLOB))+length(CAST(type AS BLOB))+length(CAST(unit AS BLOB))+coalesce(length(block),0)+length(CAST(encoding AS BLOB))+length(CAST(source AS BLOB))+length(CAST(batch AS BLOB))+length(CAST(metadata AS BLOB)))";
    Statement s(db_, "SELECT seq,id,sensor,type,unit,ts,kind,scalar,CASE WHEN " + weight + "<=:budget THEN block ELSE NULL END,quality,duration,rate,encoding,source,batch,metadata,version," + weight +
                    " FROM records WHERE ts<:end" +
                    (overlap ? " AND (ts>=:start OR (kind=2 AND duration>:start-ts))" : "") + filters(f) +
                    " AND seq<=:high AND ts>=:lower AND (ts,seq)>(:last_time,:last_seq) ORDER BY ts,seq LIMIT :count");
    auto bind = [&](const char *name, std::int64_t value) { s.bind(sqlite3_bind_parameter_index(s.stmt, name), value); };
    if (overlap) bind(":start", r.start);
    // Give SQLite a single explicit scalar lower bound. Keeping only the
    // row-value predicate beside ts>=range.start can rescan earlier pages.
    bind(":lower", std::max(overlap ? Time{0} : r.start, after.timestamp));
    bind(":end", r.end); bind(":last_time", after.timestamp); bind(":last_seq", after.sequence);
    bind(":high", highwater_); bind(":count", static_cast<std::int64_t>(limits_.page_records + 1));
    bind(":budget", static_cast<std::int64_t>(limits_.record_bytes)); bind_filters(s, f);
    Page page; page.next = after;
    std::size_t bytes = 0;
    while (s.step()) {
        budget_.check();
        // Look-ahead does not reject an oversize next record in the current page.
        if (page.records.size() == limits_.page_records) { page.more = true; break; }
        const auto size = s.integer(17);
        require(size >= 256 && static_cast<std::uint64_t>(size) <= limits_.record_bytes, "Record exceeds record byte budget");
        if (static_cast<std::uint64_t>(size) > limits_.page_bytes - bytes) {
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
} // namespace edgeecho::detail
