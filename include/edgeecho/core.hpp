#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace edgeecho {
using Time = std::int64_t; // Unix UTC microseconds, never a local time.
constexpr Time max_time = 253402300800000000LL;
struct Range { Time start = 0, end = 0; };
struct Filter { std::string sensor, type, unit; }; // Empty fields mean no filter.
struct Limits {
    std::size_t page_records = 512;
    std::size_t page_bytes = 16 * 1024 * 1024;
    std::size_t record_bytes = 8 * 1024 * 1024;
    std::size_t scan_records = 250000;
    std::size_t decode_samples = 10000000;
    std::size_t series = 4096;
    std::chrono::milliseconds query_timeout{15000};
};
using Cancel = std::shared_ptr<std::atomic_bool>;
class Error : public std::runtime_error { public: using std::runtime_error::runtime_error; };
class SourceChanged : public Error { public: using Error::Error; };
// Captured when opening an offline input; reused by every later worker task.
// The native token detects replacement even when size/mtime and DB ID match.
struct SourceIdentity {
    std::filesystem::path canonical_path;
    std::uintmax_t file_size = 0;
    std::filesystem::file_time_type modified;
    std::string native_token, database_id, generation;
};
bool operator==(const SourceIdentity &, const SourceIdentity &);
bool operator!=(const SourceIdentity &, const SourceIdentity &);
struct ExportProgress {
    std::uint64_t records_written = 0, bytes_written = 0;
    bool published = false;
};
struct ExportOptions {
    std::chrono::milliseconds timeout{30 * 60 * 1000};
    // Called on the exporting thread; callers marshal UI updates themselves.
    // Before publication, exceptions abort export. The final published observer
    // cannot undo success: its exceptions are ignored after the file is visible.
    std::function<void(const ExportProgress &)> progress;
};
const char *version() noexcept;
const char *build_id() noexcept;
struct Series {
    Filter key;
    std::uint64_t records = 0;
    Range range;
    std::string encoding; // Representative; mixed encodings are indicated as "mixed".
    int kind = -1; // 0/1/2 when homogeneous, -1 when mixed.
};
struct Overview { std::vector<Series> series; Range range; std::uint64_t records = 0; };
struct Record {
    std::int64_t sequence = 0;
    std::string id, sensor, type, unit;
    Time timestamp = 0, duration = 0;
    int kind = 0;
    std::optional<double> scalar, rate;
    std::vector<std::uint8_t> block;
    std::uint32_t quality = 0;
    std::string encoding, source, batch, metadata; // metadata retains original packed bytes.
};
struct Cursor { Time timestamp = -1; std::int64_t sequence = 0; };
struct Page { std::vector<Record> records; Cursor next; bool more = false; };
struct Event {
    std::string id, sensor, type, event_type, description;
    Time start = 0, end = 0;
    int assessment = 0, severity = 0;
    std::int64_t revision = 0;
    bool confirmed = false;
};
struct Point { Time timestamp = 0; double value = 0; bool gap = false; };
struct Plot {
    std::vector<Point> points;
    std::size_t records_scanned = 0, samples_decoded = 0, quality_skipped = 0;
    bool estimated_time = false;
};
struct AudioOptions {
    std::uint32_t sample_rate_hz = 0; // 0 keeps the stored nominal sample rate.
    double gain = 1.0;                // Linear gain multiplier in [0, 1000].
    bool remove_dc = true;
    bool normalize = true;
    std::chrono::milliseconds timeout{5 * 60 * 1000};
};
struct AudioClip {
    std::vector<std::int16_t> pcm16;
    std::uint32_t sample_rate_hz = 0;
    Range range{0, 0};
    std::size_t records_scanned = 0, samples_decoded = 0, gap_samples = 0;
    double peak_raw = 0.0;
};
struct SpectrumBin {
    double frequency_hz = 0.0;
    double magnitude = 0.0;
};
struct Spectrum {
    std::vector<SpectrumBin> bins;
    double sample_rate_hz = 0.0;
    double dominant_frequency_hz = 0.0;
    double dominant_magnitude = 0.0;
    std::size_t samples_used = 0;
};
struct Channel { std::string id, label; };
// Supported channels for a known encoding. Empty means opaque, never guessed.
std::vector<Channel> channels(const std::string &encoding, int kind = 2);
// Validates layout/rate and streams finite values (non-finite values become gaps).
// index -> estimated UTC timestamp; feature scans use the record timestamp.
void decode(const Record &, const std::string &channel,
            const std::function<void(Point)> &, const Cancel &cancel = {});
std::vector<std::pair<std::string, std::string>> unpack_metadata(const std::string &);
std::string quality_text(std::uint32_t);
std::vector<std::uint8_t> encode_wav(const AudioClip &);
Spectrum compute_spectrum(const std::vector<double> &samples, double sample_rate_hz,
                          std::size_t max_bins = 2048);

// A Database belongs to one worker thread. Only closed/checkpointed offline input
// is accepted, allowing immutable read-only access without source WAL/SHM writes.
class Database {
  public:
    explicit Database(std::filesystem::path, Limits = {}, Cancel = {},
                      std::optional<SourceIdentity> expected_source = {});
    ~Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
    Overview overview();
    Page page(const Filter &, Range, Cursor = {}, bool overlap = false);
    std::vector<Event> events(const Filter &, Range, std::size_t limit = 1000);
    Plot plot(const Filter &, Range, const std::string &channel, std::size_t buckets = 1500);
    // One database scan for all channels. Record and decoded-value budgets are
    // shared across the request; results have the same order as channel IDs.
    std::vector<Plot> plots(const Filter &, Range, const std::vector<std::string> &channels,
                            std::size_t buckets = 1500);
    // Nearest finite, good-quality original sample within range. No interpolation
    // or envelope-point lookup. Waveform timestamps remain rate-reconstructed.
    std::optional<Point> probe(const Filter &, Range, const std::string &channel, Time target);
    // Synthesizes a uniform 16-bit mono PCM audio clip for [start,end).
    // Quality gaps or missing intervals are filled with zero-amplitude silence.
    AudioClip audio(const Filter &, Range, const std::string &channel = "sound",
                    AudioOptions = {});
    // Exports a standard RIFF WAVE (.wav) 16-bit mono PCM file atomically.
    std::uint64_t export_wav(const std::filesystem::path &, const Filter &, Range,
                             const std::string &channel = "sound", AudioOptions = {});
    // Computes a Hann-windowed single-sided FFT magnitude spectrum in [start,end).
    Spectrum spectrum(const Filter &, Range, const std::string &channel,
                      std::size_t max_bins = 2048);
    // Selection is ts in [start,end), not overlapping waveform blocks.
    std::uint64_t export_csv(const std::filesystem::path &, const Filter &, Range,
                             ExportOptions = {});
    const std::filesystem::path &path() const;
    SourceIdentity identity() const;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Cross-platform audio output engine (WinMM on Windows, PulseAudio/ALSA on Linux,
// with automatic software pacing fallback in headless/offscreen environments).
class AudioPlayer {
  public:
    AudioPlayer();
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer &) = delete;
    AudioPlayer &operator=(const AudioPlayer &) = delete;
    void load(AudioClip);
    void clear();
    void play();
    void pause();
    void stop();
    void seek(Time timestamp_us);
    void set_volume(double volume); // 0.0 .. 1.0 inclusive.
    double volume() const noexcept;
    void set_muted(bool muted) noexcept;
    bool muted() const noexcept;
    bool is_playing() const noexcept;
    bool has_clip() const noexcept;
    Time position() const noexcept;
    std::uint32_t sample_rate_hz() const noexcept;
    std::string backend_name() const;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// UI/CLI independent monotonic playback clock. Does not read or emit records.
enum class PlaybackState { stopped, playing, paused, finished };
class PlaybackClock {
  public:
    explicit PlaybackClock(Range = {0, 1});
    void set_range(Range);
    void play();
    void pause();
    void stop();
    void seek(Time);
    void set_speed(double); // 0.01 .. 1000 inclusive, finite.
    Time advance(std::chrono::microseconds);
    Time position() const { return position_; }
    double speed() const { return speed_; }
    PlaybackState state() const { return state_; }
    Range range() const { return range_; }
  private:
    Range range_;
    Time position_ = 0;
    double speed_ = 1;
    long double fraction_ = 0;
    PlaybackState state_ = PlaybackState::stopped;
};

// Creates a small v3-compatible demo database; refuses to overwrite any file.
void create_demo(const std::filesystem::path &, std::size_t scalar_records = 20000);
} // namespace edgeecho
