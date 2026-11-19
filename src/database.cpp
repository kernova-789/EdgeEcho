#include "edgeecho/core.hpp"
#include "offline_archive.hpp"
#include "signal_decode.hpp"
#include "atomic_output.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>

namespace edgeecho {
namespace {
using detail::require;
using detail::validate_range;

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

Database::Database(std::filesystem::path path, Limits limits, Cancel cancel,
                   std::optional<SourceIdentity> expected_source) try
    : impl_(std::make_unique<Impl>(std::move(path), limits, std::move(cancel), expected_source)) {}
catch (const std::filesystem::filesystem_error &) {
    throw SourceChanged("Source path cannot be inspected; reopen a closed offline database");
}
Database::~Database() = default;
const std::filesystem::path &Database::path() const { return impl_->source.canonical_path; }
SourceIdentity Database::identity() const { impl_->check_source(); return impl_->source; }
Overview Database::overview() {
    auto &i = *impl_;
    i.begin_operation();
    auto result = i.overview();
    i.check_source();
    return result;
}

Page Database::page(const Filter &filter, Range range, Cursor cursor, bool overlap) {
    auto &i = *impl_;
    i.begin_operation();
    auto result = i.read_page(filter, range, cursor, overlap);
    i.check_source();
    return result;
}

std::vector<Event> Database::events(const Filter &filter, Range range, std::size_t limit) {
    auto &i = *impl_;
    i.begin_operation();
    auto result = i.events(filter, range, limit);
    i.check_source();
    return result;
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
    i.scan(f, r, [&](const detail::replay::Record &row) {
        const bool valid = row.quality == detail::replay::good &&
                           !std::holds_alternative<std::monostate>(row.value);
        std::optional<detail::replay::SignalView> signal;
        if (valid) signal.emplace(row);
        const bool waveform = signal && signal->kind() == detail::replay::SignalKind::waveform;
        for (std::size_t n = 0; n < selected.size(); ++n) {
            auto &result = results[n]; auto &envelope = envelopes[n];
            ++result.records_scanned;
            if (!signal) {
                ++result.quality_skipped; envelope.push({std::max(r.start, row.timestamp), 0, true}); continue;
            }
            if (waveform && previous_end && row.timestamp > *previous_end + 1)
                envelope.push({std::max(r.start, *previous_end), 0, true});
            if (waveform && previous_end && row.timestamp < *previous_end)
                envelope.push({std::max(r.start, row.timestamp), 0, true});
            detail::decode_window(*signal, selected[n], r, [&](Point point) {
                require(++decoded <= i.limits.decode_samples, "Plot decoded value budget exceeded; narrow the time range");
                ++result.samples_decoded;
                if ((decoded & 4095U) == 0) i.check_budget();
                envelope.push(point);
            }, i.budget.cancellation());
            if (waveform) result.estimated_time = true;
        }
        if (waveform && row.quality == 0) previous_end = row.timestamp + row.duration_us;
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
    i.scan(f, r, [&](const detail::replay::Record &row) {
        if (std::holds_alternative<std::monostate>(row.value) || row.quality != 0) return;
        detail::decode_window(detail::replay::SignalView(row), channel, r, [&](Point point) {
            require(++decoded <= i.limits.decode_samples, "Probe decoded sample budget exceeded; narrow the time range");
            if ((decoded & 4095U) == 0) i.check_budget();
            if (point.gap) return;
            const auto delta = point.timestamp >= target ? point.timestamp - target : target - point.timestamp;
            // Earlier timestamps win ties. Same-time rows retain (ts,seq) order.
            if (!closest || delta < distance || (delta == distance && point.timestamp < closest->timestamp)) {
                closest = point; distance = delta;
            }
        }, i.budget.cancellation());
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
    i.budget.reset(options.timeout);

    struct RawSample { double value = 0.0; bool is_gap = false; };
    std::vector<RawSample> raw;
    double detected_rate = 0.0;
    AudioClip clip;
    clip.range = r;
    std::size_t total_emitted = 0;
    std::optional<Time> previous_end;

    i.scan(f, r, [&](const detail::replay::Record &row) {
        ++clip.records_scanned;
        if (row.sample_rate_hz && *row.sample_rate_hz > 0.0 && detected_rate <= 0.0)
            detected_rate = *row.sample_rate_hz;
        const double rate_for_gap = detected_rate > 0.0 ? detected_rate : 1000.0;
        const Time clip_start = std::max(r.start, row.timestamp);
        const Time clip_end = std::min(r.end, row.timestamp + std::max<Time>(1, row.duration_us));
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
        if (row.quality != 0 || std::holds_alternative<std::monostate>(row.value)) {
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
        detail::decode_window(detail::replay::SignalView(row), channel, r, [&](Point point) {
            require(++total_emitted <= i.limits.decode_samples, "Audio sample budget exceeded; narrow the time range");
            if ((total_emitted & 4095U) == 0) i.check_budget();
            if (point.gap || !std::isfinite(point.value)) {
                raw.push_back({0.0, true});
                ++clip.gap_samples;
            } else {
                raw.push_back({point.value, false});
                ++clip.samples_decoded;
            }
        }, i.budget.cancellation());
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
    if (options.low_cut_hz > 0.0 || options.high_cut_hz > 0.0) {
        std::vector<double> filter_in(raw.size(), 0.0);
        for (std::size_t idx = 0; idx < raw.size(); ++idx) {
            filter_in[idx] = raw[idx].is_gap ? 0.0 : (raw[idx].value - mean);
        }
        auto filtered = filter_bandpass(filter_in, static_cast<double>(nominal_rate),
                                        options.low_cut_hz, options.high_cut_hz);
        for (std::size_t idx = 0; idx < raw.size() && idx < filtered.size(); ++idx) {
            if (!raw[idx].is_gap)
                raw[idx].value = filtered[idx];
        }
        mean = 0.0;
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
    const auto &source = i.source.canonical_path;
    require(destination != source && destination.u8string() != source.u8string() + "-wal" &&
            destination.u8string() != i.source.canonical_path.u8string() + "-shm" &&
            destination.u8string() != i.source.canonical_path.u8string() + "-journal",
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
    i.scan(f, r, [&](const detail::replay::Record &row) {
        if (std::holds_alternative<std::monostate>(row.value) || row.quality != 0) return;
        if (row.sample_rate_hz && *row.sample_rate_hz > 0.0 && detected_rate <= 0.0)
            detected_rate = *row.sample_rate_hz;
        detail::decode_window(detail::replay::SignalView(row), channel, r, [&](Point point) {
            require(++decoded <= i.limits.decode_samples, "Spectrum decoded sample budget exceeded; narrow the time range");
            if ((decoded & 4095U) == 0) i.check_budget();
            if (point.gap)
                return;
            if (samples.size() < 65536) {
                samples.push_back(point.value);
                if (timestamps.size() < 1024)
                    timestamps.push_back(point.timestamp);
            }
        }, i.budget.cancellation());
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
Spectrogram Database::spectrogram(const Filter &f, Range r, const std::string &channel,
                                  std::size_t time_bins, std::size_t freq_bins) {
    auto &i = *impl_; i.begin_operation(); validate_range(r);
    require(!f.sensor.empty() && !f.type.empty() && !f.unit.empty(), "Spectrogram requires an exact sensor/type/unit series");
    require(!channel.empty() && channel.size() <= 512, "Spectrogram requires a nonempty channel ID");
    require(time_bins >= 4 && time_bins <= 512 && freq_bins >= 4 && freq_bins <= 512,
            "Spectrogram bins must be in [4, 512]");
    std::vector<double> samples;
    samples.reserve(32768);
    std::vector<Time> timestamps;
    timestamps.reserve(1024);
    double detected_rate = 0.0;
    std::size_t decoded = 0;
    i.scan(f, r, [&](const detail::replay::Record &row) {
        if (std::holds_alternative<std::monostate>(row.value) || row.quality != 0) return;
        if (row.sample_rate_hz && *row.sample_rate_hz > 0.0 && detected_rate <= 0.0)
            detected_rate = *row.sample_rate_hz;
        detail::decode_window(detail::replay::SignalView(row), channel, r, [&](Point point) {
            require(++decoded <= i.limits.decode_samples, "Spectrogram decoded sample budget exceeded; narrow the time range");
            if ((decoded & 4095U) == 0) i.check_budget();
            if (point.gap)
                return;
            if (samples.size() < 131072) {
                samples.push_back(point.value);
                if (timestamps.size() < 1024)
                    timestamps.push_back(point.timestamp);
            }
        }, i.budget.cancellation());
    });
    require(samples.size() >= 16, "Spectrogram requires at least 16 valid samples in the selected range");
    if (detected_rate <= 0.0 && timestamps.size() >= 2) {
        const Time span = timestamps.back() - timestamps.front();
        if (span > 0)
            detected_rate = static_cast<double>(timestamps.size() - 1) * 1000000.0 / static_cast<double>(span);
    }
    if (detected_rate <= 0.0) detected_rate = 1.0;
    auto result = compute_spectrogram(samples, detected_rate, r, time_bins, freq_bins);
    i.check_source();
    return result;
}
std::uint64_t Database::export_csv(const std::filesystem::path &target, const Filter &f, Range r,
                                   ExportOptions options) {
    auto &i = *impl_; i.begin_operation(); validate_range(r);
    require(options.timeout.count() > 0 && options.timeout <= std::chrono::hours(24 * 7),
            "Export timeout must be positive and no longer than seven days");
    const auto destination = std::filesystem::weakly_canonical(std::filesystem::absolute(target));
    const auto &source = i.source.canonical_path;
    require(destination != source && destination.u8string() != source.u8string() + "-wal" && destination.u8string() != source.u8string() + "-shm" && destination.u8string() != source.u8string() + "-journal", "Cannot overwrite source database or sidecars");
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    detail::AtomicOutput output(destination);
    ExportProgress progress;
    auto check_export = [&] {
        if (i.budget.cancellation() && i.budget.cancellation()->load()) throw Error("Export cancelled");
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
