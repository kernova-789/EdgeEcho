#include "edgeecho/core.hpp"
#include "sqlite3.h"
#include <fstream>
#include <iostream>
#include <sstream>

using namespace edgeecho;
namespace {
void expect(bool ok, const std::string &message) { if (!ok) throw Error(message); }
template<class F> void changed(F action, const std::string &message) {
    bool rejected = false;
    try { action(); } catch (const SourceChanged &) { rejected = true; }
    expect(rejected, message);
}
std::string bytes(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary); std::ostringstream result; result << in.rdbuf(); return result.str();
}
}
int main(int argc, char **argv) {
    try {
        expect(argc == 2, "Session test requires a workspace-local output directory");
        const auto root = std::filesystem::u8path(argv[1]);
        std::filesystem::create_directories(root);
        const auto input = root / std::filesystem::u8path("会话 #% replay.db");
        const auto replacement = root / "replacement.db";
        std::filesystem::remove(input); std::filesystem::remove(replacement);
        create_demo(input, 1000);
        const auto original_bytes = bytes(input);
        constexpr Time t = 1793145600000000LL;
        const Filter selection{"demo-vibration", "acceleration_rms", "count"};
        SourceIdentity source;
        Cursor next;
        {
            Limits limits; limits.page_records = 7;
            Database db(input, limits);
            source = db.identity();
            expect(source.file_size == std::filesystem::file_size(input) && source.database_id.size() == 32 &&
                   !source.native_token.empty() && source.generation == "0", "Complete session identity");
            auto page = db.page(selection, {t, t + 10000000});
            expect(page.records.size() == 7 && page.more, "First page"); next = page.next;
        }
        {
            Database db(input, {}, {}, source);
            expect(db.identity() == source, "Identity survives worker handoff");
            auto page = db.page(selection, {t, t + 10000000}, next);
            expect(page.records.front().id == "scalar-7", "Same source continues without duplicates");
        }
        expect(bytes(input) == original_bytes, "Session read changed source bytes");
        for (const auto *suffix : {"-wal", "-shm", "-journal"})
            expect(!std::filesystem::exists(std::filesystem::u8path(input.u8string() + suffix)), "Session read created sidecars");
        {
            auto wrong = source; wrong.database_id.assign(32, 'a');
            changed([&] { Database db(input, {}, {}, wrong); }, "Wrong database ID must reject");
            wrong = source; wrong.generation = "999";
            changed([&] { Database db(input, {}, {}, wrong); }, "Wrong generation must reject");
        }
        // A byte-for-byte cloned DB has identical database_id, size and mtime.
        // Replacing the path between workers must still invalidate the old token.
        std::filesystem::copy_file(input, replacement);
        std::filesystem::last_write_time(replacement, source.modified);
        std::filesystem::remove(input);
        std::filesystem::rename(replacement, input);
        expect(std::filesystem::file_size(input) == source.file_size &&
               std::filesystem::last_write_time(input) == source.modified, "Replacement retains size/mtime");
        changed([&] { Database db(input, {}, {}, source); }, "Cloned same-path replacement not detected");
        SourceIdentity reopened;
        {
            Database db(input); reopened = db.identity();
            expect(reopened.database_id == source.database_id && reopened.native_token != source.native_token,
                   "Native file identity distinguishes same-ID clone");
            expect(db.page(selection, {t,t+1}).records.size() == 1, "Explicit reopen recovers");
        }
        {
            Database db(input, {}, {}, reopened);
            std::filesystem::last_write_time(input, reopened.modified + std::chrono::seconds(2));
            changed([&] { (void)db.page(selection, {t,t+1}); }, "Mutation during active session not detected");
            changed([&] { (void)db.identity(); }, "Stale identity should not be handed to another worker");
        }
        { Database db(input); reopened = db.identity(); }
        {
            std::ofstream wal(std::filesystem::u8path(input.u8string()+"-wal")); wal << "active"; wal.close();
            changed([&] { Database db(input, {}, {}, reopened); }, "Active WAL must invalidate worker handoff");
            std::filesystem::remove(std::filesystem::u8path(input.u8string()+"-wal"));
        }
        std::filesystem::remove(input);
        changed([&] { Database db(input, {}, {}, reopened); }, "Missing source must invalidate session");
        std::cout << "Offline source identity, handoff, replacement, mutation and recovery checks passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
