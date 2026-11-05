#include "edgeecho/core.hpp"

#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <system_error>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
using namespace edgeecho;

std::string quoted(const std::string &s) {
    std::string out = "\"";
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '\\' || c == '"') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) {
            out += "\\u00";
            out += hex[c >> 4]; out += hex[c & 15];
        } else out += static_cast<char>(c);
    }
    return out + '"';
}
std::string hex_bytes(const std::vector<std::uint8_t> &bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result; result.reserve(bytes.size() * 2);
    for (auto b : bytes) { result += digits[b >> 4]; result += digits[b & 15]; }
    return result;
}
std::int64_t integer(const std::string &s, const std::string &name,
                     std::int64_t low, std::int64_t high) {
    std::int64_t value = 0;
    const auto parsed = std::from_chars(s.data(), s.data() + s.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size() ||
        value < low || value > high)
        throw Error(name + " must be an integer in [" + std::to_string(low) + "," +
                    std::to_string(high) + "]");
    return value;
}
struct Options {
    std::map<std::string, std::string> values;
    std::string get(const std::string &name, std::string fallback = {}) const {
        const auto found = values.find(name);
        return found == values.end() ? fallback : found->second;
    }
    bool has(const std::string &name) const { return values.count(name) != 0; }
};
Options options(const std::vector<std::string> &args, std::size_t first,
                const std::vector<std::string> &allowed) {
    Options result;
    for (std::size_t i = first; i < args.size(); i += 2) {
        const auto &name = args[i];
        bool known = false;
        for (const auto &entry : allowed) if (name == entry) known = true;
        if (!known) throw Error("Unknown option: " + name);
        if (i + 1 >= args.size()) throw Error("Missing value for " + name);
        if (!result.values.emplace(name, args[i + 1]).second)
            throw Error("Duplicate option: " + name);
    }
    return result;
}
Filter filter(const Options &o) { return {o.get("--sensor"), o.get("--type"), o.get("--unit")}; }
Range range(const Options &o, const Overview &overview) {
    Range r = overview.records ? overview.range : Range{0, 1};
    if (o.has("--start")) r.start = integer(o.get("--start"), "--start", 0, max_time - 1);
    if (o.has("--end")) r.end = integer(o.get("--end"), "--end", 1, max_time);
    if (r.end <= r.start) throw Error("Expected a nonempty UTC interval [start,end)");
    return r;
}
void print_help() {
    std::cout << "EdgeEcho " << version() << " - offline EdgeSense Replay data player\n" <<
        "Usage:\n"
        "  edgeecho_cli inspect <database-or-directory>\n"
        "  edgeecho_cli demo <new-database> [--records N]\n"
        "  edgeecho_cli export <database> <new-csv> [filters] [--export-timeout-ms 1800000]\n"
        "  edgeecho_cli audio <database> <new-wav> [filters] [--channel sound] [--sample-rate 0]\n"
        "  edgeecho_cli records <database> [filters]\n"
        "  edgeecho_cli plot <database> [filters] [--channel value] [--buckets 1500]\n"
        "  edgeecho_cli spectrum <database> [filters] [--channel value] [--bins 2048]\n"
        "  edgeecho_cli benchmark <database> [filters] [--channel value] [--buckets 1500]\n"
        "  edgeecho_cli probe <database> [filters] --at UTC_US [--channel value]\n"
        "Filters: --sensor ID --type TYPE --unit UNIT --start UTC_US --end UTC_US\n"
        "All times are integer UTC microseconds. Ranges are [start,end).\n"
        "Input must be offline/checkpointed. Nonempty WAL files are rejected.\n"
        "records emits newline-delimited JSON, including original payload hex.\n"
        "plot emits min/max envelope points; benchmark emits its measured cost.\n";
}
void print_overview(const Overview &o) {
    std::cout << "{\"records\":" << o.records << ",\"start_us\":" << o.range.start
              << ",\"end_us\":" << o.range.end << ",\"series\":[";
    bool first = true;
    for (const auto &s : o.series) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"sensor\":" << quoted(s.key.sensor) << ",\"type\":"
                  << quoted(s.key.type) << ",\"unit\":" << quoted(s.key.unit)
                  << ",\"records\":" << s.records << ",\"start_us\":" << s.range.start
                  << ",\"end_us\":" << s.range.end << ",\"encoding\":" << quoted(s.encoding) << '}';
    }
    std::cout << "]}\n";
}
void print_record(const Record &r) {
    std::cout << "{\"sequence\":" << r.sequence << ",\"id\":" << quoted(r.id)
              << ",\"sensor\":" << quoted(r.sensor) << ",\"type\":" << quoted(r.type)
              << ",\"unit\":" << quoted(r.unit) << ",\"timestamp_us\":" << r.timestamp
              << ",\"kind\":" << r.kind << ",\"scalar\":";
    if (r.scalar) std::cout << *r.scalar; else std::cout << "null";
    std::cout << ",\"quality\":" << r.quality << ",\"duration_us\":" << r.duration
              << ",\"sample_rate_hz\":";
    if (r.rate) std::cout << *r.rate; else std::cout << "null";
    std::cout << ",\"encoding\":" << quoted(r.encoding) << ",\"source\":" << quoted(r.source)
              << ",\"batch\":" << quoted(r.batch) << ",\"metadata_packed\":" << quoted(r.metadata)
              << ",\"block_hex\":" << quoted(hex_bytes(r.block)) << "}\n";
}
int execute(const std::vector<std::string> &args) {
    std::cout << std::setprecision(17);
    if (args.empty()) { print_help(); return 0; }
    if (args[0] == "--help" || args[0] == "-h") {
        if (args.size() != 1) throw Error("--help accepts no other arguments");
        print_help(); return 0;
    }
    if (args[0] == "--version") {
        if (args.size() != 1) throw Error("--version accepts no other arguments");
        std::cout << "EdgeEcho " << version() << "\nBuild: " << build_id() << '\n'; return 0;
    }
    const auto &command = args[0];
    if (command != "inspect" && command != "demo" && command != "export" && command != "audio" &&
        command != "records" && command != "plot" && command != "spectrum" &&
        command != "benchmark" && command != "probe")
        throw Error("Unknown command: " + command + "; use --help");
    if (args.size() < 2) throw Error("Missing database path; use --help");
    if (command == "demo") {
        const auto o = options(args, 2, {"--records"});
        const auto count = integer(o.get("--records", "20000"), "--records", 1, 10000000);
        create_demo(std::filesystem::u8path(args[1]), static_cast<std::size_t>(count));
        std::cout << "{\"created\":" << quoted(args[1]) << ",\"scalar_records\":" << count << "}\n";
        return 0;
    }
    if (command == "inspect") {
        if (args.size() != 2) throw Error("inspect accepts exactly one database path");
        Database db(std::filesystem::u8path(args[1]));
        print_overview(db.overview()); return 0;
    }
    const std::size_t first = (command == "export" || command == "audio") ? 3 : 2;
    if (args.size() < first) throw Error(command == "audio" ? "Missing output WAV path" : "Missing output CSV path");
    std::vector<std::string> allowed{"--sensor", "--type", "--unit", "--start", "--end"};
    if (command == "plot" || command == "benchmark") {
        allowed.push_back("--channel"); allowed.push_back("--buckets");
    }
    if (command == "spectrum") {
        allowed.push_back("--channel"); allowed.push_back("--bins");
    }
    if (command == "audio") {
        allowed.push_back("--channel"); allowed.push_back("--sample-rate");
    }
    if (command == "export") allowed.push_back("--export-timeout-ms");
    if (command == "probe") { allowed.push_back("--at"); allowed.push_back("--channel"); }
    const auto o = options(args, first, allowed);
    Database db(std::filesystem::u8path(args[1]));
    // Explicit selections can seek/export without a full-database inventory.
    // Discover series/counts only when bounds or a single-series default need it.
    Overview overview;
    const auto requested_filter = filter(o);
    const bool infer_series = (command == "plot" || command == "spectrum" || command == "audio" ||
                               command == "benchmark" || command == "probe") &&
        requested_filter.sensor.empty() && requested_filter.type.empty() && requested_filter.unit.empty();
    if (!o.has("--start") || !o.has("--end") || infer_series) overview = db.overview();
    const auto r = range(o, overview);
    auto f = filter(o);
    if (command == "export") {
        ExportOptions export_options;
        if (o.has("--export-timeout-ms"))
            export_options.timeout = std::chrono::milliseconds(integer(o.get("--export-timeout-ms"), "--export-timeout-ms", 1, 86400000));
        const auto count = db.export_csv(std::filesystem::u8path(args[2]), f, r, export_options);
        std::cout << "{\"exported_records\":" << count << ",\"path\":" << quoted(args[2]) << "}\n";
    } else if (command == "records") {
        Cursor cursor;
        for (;;) {
            const auto page = db.page(f, r, cursor);
            for (const auto &record : page.records) print_record(record);
            if (!std::cout) throw Error("Output stream failed");
            if (!page.more) break;
            cursor = page.next;
        }
    } else {
        if (f.sensor.empty() && f.type.empty() && f.unit.empty() && overview.series.size() == 1)
            f = overview.series.front().key;
        if (command == "audio" && f.sensor.empty() && f.type.empty() && f.unit.empty()) {
            for (const auto &s : overview.series) {
                if (s.encoding == "iio-le-16-v1") { f = s.key; break; }
            }
        }
        if (f.sensor.empty() || f.type.empty() || f.unit.empty())
            throw Error(command + " requires --sensor, --type and --unit to select one series");
        if (command == "audio") {
            AudioOptions audio_opts;
            if (o.has("--sample-rate")) {
                const auto sr = integer(o.get("--sample-rate"), "--sample-rate", 0, 192000);
                if (sr != 0 && sr < 100) throw Error("--sample-rate must be 0 or in [100,192000]");
                audio_opts.sample_rate_hz = static_cast<std::uint32_t>(sr);
            }
            const auto channel = o.get("--channel", "sound");
            const auto samples = db.export_wav(std::filesystem::u8path(args[2]), f, r, channel, audio_opts);
            std::cout << "{\"exported_samples\":" << samples << ",\"path\":" << quoted(args[2]) << "}\n";
            return std::cout ? 0 : 1;
        }
        if (command == "spectrum") {
            const auto bins = integer(o.get("--bins", "2048"), "--bins", 4, 65536);
            const auto s = db.spectrum(f, r, o.get("--channel", "value"), static_cast<std::size_t>(bins));
            std::cout << "{\"sample_rate_hz\":" << s.sample_rate_hz
                      << ",\"samples_used\":" << s.samples_used
                      << ",\"dominant_frequency_hz\":" << s.dominant_frequency_hz
                      << ",\"dominant_magnitude\":" << s.dominant_magnitude
                      << ",\"bins\":" << s.bins.size() << "}\n";
            for (const auto &bin : s.bins)
                std::cout << "{\"frequency_hz\":" << bin.frequency_hz
                          << ",\"magnitude\":" << bin.magnitude << "}\n";
            return std::cout ? 0 : 1;
        }
        if (command == "probe") {
            if (!o.has("--at")) throw Error("probe requires --at UTC_US");
            const auto target = integer(o.get("--at"), "--at", r.start, r.end - 1);
            const auto point = db.probe(f, r, o.get("--channel", "value"), target);
            std::cout << "{\"found\":" << (point ? "true" : "false") << ",\"target_us\":" << target;
            if (point) std::cout << ",\"timestamp_us\":" << point->timestamp << ",\"value\":" << point->value;
            std::cout << ",\"interpolated\":false}\n";
            return std::cout ? 0 : 1;
        }
        const auto buckets = integer(o.get("--buckets", "1500"), "--buckets", 1, 100000);
        const auto started = std::chrono::steady_clock::now();
        const auto p = db.plot(f, r, o.get("--channel", "value"), static_cast<std::size_t>(buckets));
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        std::cout << "{\"records_scanned\":" << p.records_scanned << ",\"samples_decoded\":"
                  << p.samples_decoded << ",\"quality_skipped\":" << p.quality_skipped
                  << ",\"points\":" << p.points.size() << ",\"elapsed_ms\":" << elapsed
                  << ",\"estimated_time\":" << (p.estimated_time ? "true" : "false") << "}\n";
        if (command == "plot") {
            for (const auto &point : p.points)
                std::cout << "{\"timestamp_us\":" << point.timestamp << ",\"value\":"
                          << point.value << ",\"gap\":" << (point.gap ? "true" : "false") << "}\n";
        }
    }
    if (!std::cout) throw Error("Output stream failed");
    return 0;
}
int guarded(const std::vector<std::string> &args) {
    try { return execute(args); }
    catch (const std::exception &e) { std::cerr << "EdgeEcho: " << e.what() << '\n'; return 1; }
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(std::filesystem::path(argv[i]).u8string());
    return guarded(args);
}
#else
int main(int argc, char **argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return guarded(args);
}
#endif
