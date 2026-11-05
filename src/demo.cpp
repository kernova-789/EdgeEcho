#include "edgeecho/core.hpp"
#include "demo_schema.hpp"
#include "sqlite3.h"
#include "atomic_output.hpp"

#include <cmath>
#include <cstring>
#include <system_error>

namespace edgeecho {
namespace {
void check(int rc, sqlite3 *db) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE) throw Error(std::string("Demo SQLite: ") + sqlite3_errmsg(db));
}
void put(std::vector<std::uint8_t> &b, std::size_t pos, std::uint64_t value, unsigned count) {
    for (unsigned j = 0; j < count; ++j) b.at(pos+j) = static_cast<std::uint8_t>((value >> (j*8)) & 255U);
}
}
void create_demo(const std::filesystem::path &target, std::size_t count) {
    if (count == 0 || count > 10000000) throw Error("Demo record count must be in 1..10000000");
    detail::AtomicOutput output(std::filesystem::weakly_canonical(std::filesystem::absolute(target)));
    const auto private_path = output.prepare_external_write();
    sqlite3 *db = nullptr;
    sqlite3_stmt *stmt = nullptr;
    try {
        const auto bytes = private_path.u8string();
        check(sqlite3_open_v2(bytes.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr), db);
        check(sqlite3_exec(db, demo_schema, nullptr, nullptr, nullptr), db);
        check(sqlite3_exec(db, "UPDATE meta SET value=lower(hex(randomblob(16))) WHERE key='database_id';", nullptr, nullptr, nullptr), db);
        check(sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr), db);
        const char *sql = "INSERT INTO records(id,sensor,type,unit,ts,kind,scalar,block,quality,duration,rate,encoding,source,batch,metadata,version) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,'demo','12:input_origin9:simulated',1)";
        check(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db);
        auto text = [&](int n, const std::string &v) { check(sqlite3_bind_text(stmt,n,v.c_str(),static_cast<int>(v.size()),SQLITE_TRANSIENT),db); };
        auto integer = [&](int n, Time v) { check(sqlite3_bind_int64(stmt,n,v),db); };
        constexpr Time anchor = 1793145600000000LL; // 2026-10-28 00:00:00 UTC.
        for (std::size_t n = 0; n < count; ++n) {
            sqlite3_reset(stmt); sqlite3_clear_bindings(stmt);
            text(1, "scalar-"+std::to_string(n)); text(2,"demo-vibration"); text(3,"acceleration_rms"); text(4,"count");
            integer(5, anchor + static_cast<Time>(n) * 10000); integer(6,1);
            check(sqlite3_bind_double(stmt,7,10+std::sin(static_cast<double>(n)*0.035)*4+(n%997==0?30:0)),db);
            integer(9,n%503==502?4:0); integer(10,0); text(12,""); text(13,"EdgeEcho demo (synthetic)"); check(sqlite3_step(stmt),db);
        }
        // 20 seconds each of representative raw IIO vibration, sound, feature.
        for (int kind = 0; kind < 3; ++kind) {
            const std::size_t scans = kind==2 ? 1 : 1000;
            const std::size_t stride = kind==0 ? 24 : kind==1 ? 16 : 72;
            for (int batch = 0; batch < 20; ++batch) {
                std::vector<std::uint8_t> block(scans*stride,0);
                for (std::size_t s = 0; s < scans; ++s) {
                    const auto t = static_cast<double>(batch) + static_cast<double>(s)/1000;
                    if (kind==0) {
                        for (int axis=0;axis<3;++axis) {
                            const auto value = static_cast<std::int16_t>(10000*std::sin(t*6.283185307179586*(5+axis)));
                            put(block,s*stride+static_cast<std::size_t>(axis)*2,static_cast<std::uint16_t>(value),2);
                        }
                        put(block,s*stride+8,static_cast<unsigned>(batch),4); put(block,s*stride+12,s,2);
                        put(block,s*stride+16,static_cast<std::uint64_t>(anchor + batch*1000000LL + static_cast<Time>(s)*1000)*1000,8);
                    } else if (kind==1) {
                        auto value = static_cast<std::int32_t>(1000000*std::sin(t*6.283185307179586*80));
                        put(block,s*stride,static_cast<std::uint32_t>(value),4);
                        put(block,s*stride+4,static_cast<unsigned>(batch),2); put(block,s*stride+6,s,2);
                        put(block,s*stride+8,static_cast<std::uint64_t>(anchor + batch*1000000LL + static_cast<Time>(s)*1000)*1000,8);
                    } else {
                        for (unsigned c=0;c<28;++c) put(block,c*2,100+c*20+static_cast<unsigned>(batch),2);
                        put(block,12,static_cast<std::uint16_t>(2500+batch),2); put(block,56,3,1);
                        put(block,64,static_cast<std::uint64_t>(anchor + batch*1000000LL)*1000,8);
                    }
                }
                sqlite3_reset(stmt); sqlite3_clear_bindings(stmt);
                text(1,"block-"+std::to_string(kind)+"-"+std::to_string(batch));
                text(2,kind==1?"demo-sound":"demo-vibration");
                text(3,kind==0?"vibration_iio_block":kind==1?"raw_sound_iio":"feature_iio_scan"); text(4,"count");
                integer(5,anchor+batch*1000000LL); integer(6,2);
                check(sqlite3_bind_blob(stmt,8,block.data(),static_cast<int>(block.size()),SQLITE_TRANSIENT),db);
                integer(9,0); integer(10,kind==2?1:1000000);
                if (kind!=2) check(sqlite3_bind_double(stmt,11,1000),db);
                text(12,kind==0?"iio-le-24-v1":kind==1?"iio-le-16-v1":"iio-le-72-v1"); text(13,"EdgeEcho demo (synthetic)"); check(sqlite3_step(stmt),db);
            }
        }
        sqlite3_finalize(stmt); stmt = nullptr;
        check(sqlite3_exec(db,
            "INSERT INTO events(id,sensor,type,start,end,assessment,severity,event_type,description,source,model,version) VALUES('demo-event','demo-vibration','',1793145603000000,1793145607000000,2,1,'demo_warning','Synthetic event for replay demonstration','EdgeEcho','demo',1);"
            "INSERT INTO event_history(id,sensor,type,start,end,assessment,severity,event_type,description,source,model,version,revision,previous_revision,action,operation_id,operated_at,actor,reason,revoked,confirmed,confirmed_by,note,request) SELECT id,sensor,type,start,end,assessment,severity,event_type,description,source,model,version,1,0,0,'demo-create',start,'demo','synthetic',0,0,'','','' FROM events;"
            "UPDATE aggregate_state SET backfill_end=(SELECT coalesce(max(seq),0) FROM records),backfill_cursor=0;"
            "COMMIT;",nullptr,nullptr,nullptr),db);
        check(sqlite3_close(db),db); db = nullptr;
        output.publish();
    } catch (...) {
        if (stmt) sqlite3_finalize(stmt);
        if (db) sqlite3_close(db);
        throw;
    }
}
} // namespace edgeecho
