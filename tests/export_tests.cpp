#include "edgeecho/core.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

using namespace edgeecho;
namespace {
void expect(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void fails(F operation, const std::string &message) {
    bool failed = false;
    try { operation(); } catch (const std::exception &) { failed = true; }
    expect(failed, message);
}
std::string read(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary); std::ostringstream content;
    content << input.rdbuf(); return content.str();
}
void write(const std::filesystem::path &path, const std::string &text) {
    std::ofstream output(path, std::ios::binary); output << text;
    expect(static_cast<bool>(output), "fixture output failed");
}
void no_private_files(const std::filesystem::path &root) {
    for (const auto &entry : std::filesystem::directory_iterator(root))
        expect(entry.path().filename().u8string().find(".edgeecho-export-") != 0,
               "private output leaked: " + entry.path().filename().u8string());
}
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned waiting = 0;
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        ++waiting; changed.notify_all();
        if (!changed.wait_for(lock, std::chrono::seconds(10), [&] { return waiting == 2; }))
            throw std::runtime_error("export race barrier timed out");
    }
};
} // namespace
int main(int argc, char **argv) {
    try {
        expect(argc == 2, "test needs a dedicated output directory inside EdgeEcho");
        const auto root = std::filesystem::absolute(std::filesystem::u8path(argv[1]));
        std::filesystem::create_directories(root);
        // Only files created by this test are removed on rerun.
        const auto source = root / "export-source.db";
        std::filesystem::remove(source); create_demo(source, 100);
        const auto original = read(source);
        constexpr Time start = 1793145600000000LL;
        const Range range{start, start + 1000000};
        const Filter scalar{"demo-vibration", "acceleration_rms", "count"};
        auto fresh = [&](const std::string &name) {
            auto path = root / std::filesystem::u8path(name);
            std::filesystem::remove(path); return path;
        };
        {
            Database db(source); const auto target = fresh("导出 # %.csv");
            ExportProgress previous; bool published = false; std::size_t observations = 0;
            ExportOptions options;
            options.progress = [&](const ExportProgress &progress) {
                expect(progress.records_written >= previous.records_written && progress.bytes_written >= previous.bytes_written,
                       "export progress went backwards");
                if (!progress.published) expect(!std::filesystem::exists(target), "final filename visible before completion");
                previous = progress; ++observations;
                if (progress.published) { published = true; throw Error("observer after completion must not revoke publication"); }
            };
            expect(db.export_csv(target, scalar, range, options) == 100, "normal export record count");
            expect(published && observations >= 102, "missing progress/completion notification");
            expect(previous.bytes_written == std::filesystem::file_size(target), "published byte count mismatch");
            const auto content = read(target);
            expect(content.find("sample_rate_hz") != std::string::npos && content.find("scalar-99") != std::string::npos,
                   "normal CSV lost payload");
        }
        no_private_files(root);
        {
            Database db(source); const auto target = fresh("existing.csv"); write(target, "existing user output");
            fails([&] { db.export_csv(target, scalar, range); }, "existing CSV must not be replaced");
            expect(read(target) == "existing user output", "existing target changed");
            const auto alias = fresh("source-hardlink.csv");
            std::error_code ec; std::filesystem::create_hard_link(source, alias, ec);
            if (!ec) {
                fails([&] { db.export_csv(alias, scalar, range); }, "hardlink source alias accepted");
                expect(read(source) == original, "source alias changed database");
                std::filesystem::remove(alias);
            }
        }
        no_private_files(root);
        {
            Database db(source); const auto target = fresh("raced-user.csv"); bool created = false;
            ExportOptions options;
            options.progress = [&](const ExportProgress &progress) {
                if (!progress.published && !created) { write(target, "concurrent user output"); created = true; }
            };
            fails([&] { db.export_csv(target, scalar, range, options); }, "raced destination unexpectedly replaced");
            expect(read(target) == "concurrent user output", "failed publication deleted/replaced another writer's file");
        }
        no_private_files(root);
        {
            const auto target = fresh("parallel.csv"); Gate gate; std::atomic<unsigned> successes{0}, failures{0};
            auto exporter = [&] {
                try {
                    Database db(source); ExportOptions options; bool entered = false;
                    options.progress = [&](const ExportProgress &progress) {
                        if (!progress.published && !entered) { entered = true; gate.wait(); }
                    };
                    const auto count = db.export_csv(target, scalar, range, options);
                    expect(count == 100, "parallel export count"); ++successes;
                } catch (const std::exception &) { ++failures; }
            };
            std::thread first(exporter), second(exporter); first.join(); second.join();
            expect(successes == 1 && failures == 1, "exactly one concurrent exporter must publish");
            expect(read(target).find("scalar-99") != std::string::npos, "parallel winner is incomplete");
        }
        no_private_files(root);
        {
            const auto target = fresh("cancelled.csv"); auto cancel = std::make_shared<std::atomic_bool>(false);
            Database db(source, {}, cancel); ExportOptions options; bool published = false;
            options.progress = [&](const ExportProgress &progress) {
                published = progress.published;
                if (progress.records_written >= 3) cancel->store(true);
            };
            fails([&] { db.export_csv(target, scalar, range, options); }, "cancellation must stop export");
            expect(!published && !std::filesystem::exists(target), "cancelled export appeared complete");
        }
        no_private_files(root);
        {
            const auto target = fresh("cancelled-block.csv"); auto cancel = std::make_shared<std::atomic_bool>(false);
            Database db(source, {}, cancel); ExportOptions options; bool cancelled_mid_record = false;
            options.progress = [&](const ExportProgress &progress) {
                if (!progress.published && progress.records_written == 0 && progress.bytes_written > 10000) {
                    cancelled_mid_record = true; cancel->store(true);
                }
            };
            fails([&] { db.export_csv(target, {"demo-vibration", "vibration_iio_block", "count"}, range, options); },
                  "block-chunk cancellation must stop export");
            expect(cancelled_mid_record && !std::filesystem::exists(target), "block did not expose cancellable chunk progress");
        }
        no_private_files(root);
        {
            Database db(source); const auto target = fresh("observer-failure.csv"); ExportOptions options;
            options.progress = [&](const ExportProgress &progress) {
                if (progress.records_written == 2) throw Error("injected output observer failure");
            };
            fails([&] { db.export_csv(target, scalar, range, options); }, "observer failure must abort unpublished export");
            expect(!std::filesystem::exists(target), "failed export has a final filename");
            fails([&] { db.export_csv(root / "missing-parent" / "out.csv", scalar, range); }, "missing directory must fail");
        }
        no_private_files(root);
        {
            Database db(source); const auto target = fresh("expired.csv"); ExportOptions options;
            options.timeout = std::chrono::milliseconds(20);
            options.progress = [](const ExportProgress &) { std::this_thread::sleep_for(std::chrono::milliseconds(40)); };
            fails([&] { db.export_csv(target, scalar, range, options); }, "total export deadline must apply outside SQL work");
            expect(!std::filesystem::exists(target), "expired export published");
            options.timeout = std::chrono::milliseconds(0);
            fails([&] { db.export_csv(target, scalar, range, options); }, "nonpositive timeout accepted");
        }
        no_private_files(root);
        {
            Limits limits; limits.query_timeout = std::chrono::milliseconds(200); limits.page_records = 1;
            Database db(source, limits); const auto target = fresh("longer-than-query.csv"); ExportOptions options;
            options.timeout = std::chrono::seconds(5); std::uint64_t last = 0;
            options.progress = [&](const ExportProgress &progress) {
                if (!progress.published && progress.records_written > last) {
                    last = progress.records_written;
                    std::this_thread::sleep_for(std::chrono::milliseconds(60));
                }
            };
            const auto began = std::chrono::steady_clock::now();
            expect(db.export_csv(target, scalar, {start, start + 50000}, options) == 5, "long export count");
            expect(std::chrono::steady_clock::now() - began > limits.query_timeout,
                   "long-export fixture did not exceed interactive query budget");
            expect(std::filesystem::exists(target), "independent export deadline did not permit long work");
        }
        no_private_files(root);
        {
            const auto target = fresh("demo-existing.db"); write(target, "existing database-like file");
            fails([&] { create_demo(target, 20); }, "demo overwrote existing file");
            expect(read(target) == "existing database-like file", "demo changed existing target");
            const auto race = fresh("demo-parallel.db"); std::atomic<unsigned> successes{0}, failures{0};
            auto maker = [&] { try { create_demo(race, 20); ++successes; } catch (const std::exception &) { ++failures; } };
            std::thread first(maker), second(maker); first.join(); second.join();
            expect(successes == 1 && failures == 1, "only one demo writer may publish");
            Database db(race); expect(db.overview().records == 80, "published demo database is incomplete");
        }
        no_private_files(root);
        {
            Database db(source);
            const Filter sound_filter{"demo-sound", "raw_sound_iio", "count"};
            const Range sound_range{start, start + 2000000};
            const auto clip = db.audio(sound_filter, sound_range, "sound");
            expect(clip.sample_rate_hz == 1000 && clip.pcm16.size() == 2000 && clip.samples_decoded == 2000,
                   "sound series audio synthesis");
            const auto spec = db.spectrum(sound_filter, sound_range, "sound", 512);
            expect(std::abs(spec.dominant_frequency_hz - 80.0) < 1.5, "sound series FFT dominant frequency");
            const auto wav_out = fresh("sound.wav");
            expect(db.export_wav(wav_out, sound_filter, sound_range, "sound") == 2000, "WAV export sample count");
            const auto wav_bytes = read(wav_out);
            expect(wav_bytes.size() == 44 + 2000 * 2 && wav_bytes.substr(0, 4) == "RIFF" && wav_bytes.substr(8, 4) == "WAVE",
                   "exported WAV header and byte length");
            fails([&] { db.export_wav(wav_out, sound_filter, sound_range, "sound"); }, "WAV export must not overwrite existing file");
            fails([&] { db.export_wav(source, sound_filter, sound_range, "sound"); }, "WAV export must not overwrite source database");
        }
        no_private_files(root);
        expect(read(source) == original, "exports changed source database");
        for (const auto *tail : {"-wal", "-shm", "-journal"})
            expect(!std::filesystem::exists(std::filesystem::u8path(source.u8string() + tail)), "source sidecar created");
        std::cout << "Atomic export/demo, race, progress, cancellation, independent deadline and source safety passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
