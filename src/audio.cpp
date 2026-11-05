#include "edgeecho/core.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

namespace edgeecho {
namespace {
constexpr double pi = 3.14159265358979323846;

void require(bool condition, const std::string &message) {
    if (!condition)
        throw Error(message);
}

void append_le16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void append_le32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void fft_in_place(std::vector<std::complex<double>> &a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1U;
        for (; j & bit; bit >>= 1U)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1U) {
        const double angle = -2.0 * pi / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            const std::size_t half = len >> 1U;
            for (std::size_t j = 0; j < half; ++j) {
                const std::complex<double> u = a[i + j];
                const std::complex<double> v = a[i + j + half] * w;
                a[i + j] = u + v;
                a[i + j + half] = u - v;
                w *= wlen;
            }
        }
    }
}

std::vector<std::int16_t> resample_pcm16(const std::vector<std::int16_t> &input,
                                         std::uint32_t in_rate,
                                         std::uint32_t out_rate) {
    if (input.empty() || in_rate == 0 || out_rate == 0 || in_rate == out_rate)
        return input;
    const auto out_count = static_cast<std::size_t>(std::clamp<std::uint64_t>(
        (static_cast<std::uint64_t>(input.size()) * out_rate + in_rate / 2U) / in_rate,
        1ULL, 48000ULL * 600ULL));
    std::vector<std::int16_t> output(out_count, 0);
    if (input.size() == 1) {
        std::fill(output.begin(), output.end(), input.front());
        return output;
    }
    const double ratio = static_cast<double>(in_rate) / static_cast<double>(out_rate);
    for (std::size_t i = 0; i < out_count; ++i) {
        const double pos = static_cast<double>(i) * ratio;
        const auto idx = static_cast<std::size_t>(pos);
        if (idx + 1 >= input.size()) {
            output[i] = input.back();
        } else {
            const double frac = pos - static_cast<double>(idx);
            const double v = static_cast<double>(input[idx]) * (1.0 - frac) +
                             static_cast<double>(input[idx + 1]) * frac;
            output[i] = static_cast<std::int16_t>(
                std::clamp(std::lround(v), -32768L, 32767L));
        }
    }
    return output;
}

bool prefer_null_audio() {
    if (const char *backend = std::getenv("EDGEECHO_AUDIO_BACKEND")) {
        if (std::string(backend) == "null" || std::string(backend) == "offscreen")
            return true;
    }
    if (const char *platform = std::getenv("QT_QPA_PLATFORM")) {
        if (std::string(platform) == "offscreen")
            return true;
    }
    return false;
}

#ifdef _WIN32
class WinMmSink {
  public:
    explicit WinMmSink(std::uint32_t rate) {
        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 1;
        wfx.nSamplesPerSec = static_cast<DWORD>(rate);
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = 2;
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * 2;
        if (waveOutOpen(&handle_, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR)
            open_ = true;
    }
    ~WinMmSink() {
        if (open_ && handle_) {
            waveOutReset(handle_);
            waveOutClose(handle_);
        }
    }
    bool is_open() const noexcept { return open_; }
    bool write(const std::int16_t *samples, std::size_t count, const std::atomic_bool &stop_flag) {
        if (!open_ || !handle_ || count == 0)
            return false;
        std::vector<std::int16_t> copy(samples, samples + count);
        WAVEHDR hdr{};
        hdr.lpData = reinterpret_cast<LPSTR>(copy.data());
        hdr.dwBufferLength = static_cast<DWORD>(copy.size() * sizeof(std::int16_t));
        if (waveOutPrepareHeader(handle_, &hdr, sizeof(hdr)) != MMSYSERR_NOERROR)
            return false;
        if (waveOutWrite(handle_, &hdr, sizeof(hdr)) != MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(handle_, &hdr, sizeof(hdr));
            return false;
        }
        while (!(hdr.dwFlags & WHDR_DONE)) {
            if (stop_flag.load(std::memory_order_relaxed)) {
                waveOutReset(handle_);
                break;
            }
            Sleep(2);
        }
        waveOutUnprepareHeader(handle_, &hdr, sizeof(hdr));
        return true;
    }
    void drop() {
        if (open_ && handle_)
            waveOutReset(handle_);
    }
  private:
    HWAVEOUT handle_ = nullptr;
    bool open_ = false;
};
#elif defined(__linux__)
struct PaSampleSpec {
    int format;
    std::uint32_t rate;
    std::uint8_t channels;
};
struct PaBufferAttr {
    std::uint32_t maxlength;
    std::uint32_t tlength;
    std::uint32_t prebuf;
    std::uint32_t minreq;
    std::uint32_t fragsize;
};

class PulseSink {
  public:
    explicit PulseSink(std::uint32_t rate) {
        lib_ = dlopen("libpulse-simple.so.0", RTLD_LAZY | RTLD_LOCAL);
        if (!lib_)
            return;
        new_fn_ = reinterpret_cast<NewFn>(dlsym(lib_, "pa_simple_new"));
        write_fn_ = reinterpret_cast<WriteFn>(dlsym(lib_, "pa_simple_write"));
        flush_fn_ = reinterpret_cast<FlushFn>(dlsym(lib_, "pa_simple_flush"));
        free_fn_ = reinterpret_cast<FreeFn>(dlsym(lib_, "pa_simple_free"));
        if (!new_fn_ || !write_fn_ || !flush_fn_ || !free_fn_)
            return;
        PaSampleSpec spec{3 /* PA_SAMPLE_S16LE */, rate, 1};
        const std::uint32_t bytes_per_50ms = std::max<std::uint32_t>(rate / 20U * 2U, 512U);
        PaBufferAttr attr{bytes_per_50ms * 4U, bytes_per_50ms,
                          static_cast<std::uint32_t>(-1),
                          static_cast<std::uint32_t>(-1),
                          static_cast<std::uint32_t>(-1)};
        int error = 0;
        stream_ = new_fn_(nullptr, "EdgeEcho", 1 /* PA_STREAM_PLAYBACK */, nullptr,
                          "Sensor Replay Audio", &spec, nullptr, &attr, &error);
    }
    ~PulseSink() {
        if (stream_ && free_fn_)
            free_fn_(stream_);
        if (lib_)
            dlclose(lib_);
    }
    bool is_open() const noexcept { return stream_ != nullptr; }
    bool write(const std::int16_t *samples, std::size_t count) {
        if (!stream_ || !write_fn_ || count == 0)
            return false;
        int error = 0;
        return write_fn_(stream_, samples, count * sizeof(std::int16_t), &error) >= 0;
    }
    void drop() {
        if (stream_ && flush_fn_) {
            int error = 0;
            flush_fn_(stream_, &error);
        }
    }
  private:
    using NewFn = void *(*)(const char *, const char *, int, const char *, const char *,
                            const PaSampleSpec *, const void *, const PaBufferAttr *, int *);
    using WriteFn = int (*)(void *, const void *, std::size_t, int *);
    using FlushFn = int (*)(void *, int *);
    using FreeFn = void (*)(void *);
    void *lib_ = nullptr;
    void *stream_ = nullptr;
    NewFn new_fn_ = nullptr;
    WriteFn write_fn_ = nullptr;
    FlushFn flush_fn_ = nullptr;
    FreeFn free_fn_ = nullptr;
};

class AlsaSink {
  public:
    explicit AlsaSink(std::uint32_t rate) {
        lib_ = dlopen("libasound.so.2", RTLD_LAZY | RTLD_LOCAL);
        if (!lib_)
            return;
        open_fn_ = reinterpret_cast<OpenFn>(dlsym(lib_, "snd_pcm_open"));
        params_fn_ = reinterpret_cast<ParamsFn>(dlsym(lib_, "snd_pcm_set_params"));
        writei_fn_ = reinterpret_cast<WriteiFn>(dlsym(lib_, "snd_pcm_writei"));
        recover_fn_ = reinterpret_cast<RecoverFn>(dlsym(lib_, "snd_pcm_recover"));
        prepare_fn_ = reinterpret_cast<PrepareFn>(dlsym(lib_, "snd_pcm_prepare"));
        drop_fn_ = reinterpret_cast<DropFn>(dlsym(lib_, "snd_pcm_drop"));
        close_fn_ = reinterpret_cast<CloseFn>(dlsym(lib_, "snd_pcm_close"));
        if (!open_fn_ || !params_fn_ || !writei_fn_ || !recover_fn_ ||
            !prepare_fn_ || !drop_fn_ || !close_fn_)
            return;
        if (open_fn_(&pcm_, "default", 0 /* SND_PCM_STREAM_PLAYBACK */, 0) < 0 || !pcm_) {
            pcm_ = nullptr;
            return;
        }
        // SND_PCM_FORMAT_S16_LE = 2, SND_PCM_ACCESS_RW_INTERLEAVED = 3
        if (params_fn_(pcm_, 2, 3, 1U, rate, 1, 50000U) < 0) {
            close_fn_(pcm_);
            pcm_ = nullptr;
        }
    }
    ~AlsaSink() {
        if (pcm_ && close_fn_) {
            if (drop_fn_)
                drop_fn_(pcm_);
            close_fn_(pcm_);
        }
        if (lib_)
            dlclose(lib_);
    }
    bool is_open() const noexcept { return pcm_ != nullptr; }
    bool write(const std::int16_t *samples, std::size_t count) {
        if (!pcm_ || !writei_fn_ || count == 0)
            return false;
        auto frames = static_cast<long>(writei_fn_(pcm_, samples, count));
        if (frames < 0 && recover_fn_)
            frames = static_cast<long>(recover_fn_(pcm_, static_cast<int>(frames), 1));
        return frames >= 0;
    }
    void drop() {
        if (pcm_ && drop_fn_ && prepare_fn_) {
            drop_fn_(pcm_);
            prepare_fn_(pcm_);
        }
    }
  private:
    using OpenFn = int (*)(void **, const char *, int, int);
    using ParamsFn = int (*)(void *, int, int, unsigned int, unsigned int, int, unsigned int);
    using WriteiFn = long (*)(void *, const void *, unsigned long);
    using RecoverFn = int (*)(void *, int, int);
    using PrepareFn = int (*)(void *);
    using DropFn = int (*)(void *);
    using CloseFn = int (*)(void *);
    void *lib_ = nullptr;
    void *pcm_ = nullptr;
    OpenFn open_fn_ = nullptr;
    ParamsFn params_fn_ = nullptr;
    WriteiFn writei_fn_ = nullptr;
    RecoverFn recover_fn_ = nullptr;
    PrepareFn prepare_fn_ = nullptr;
    DropFn drop_fn_ = nullptr;
    CloseFn close_fn_ = nullptr;
};
#endif
} // namespace

std::vector<std::uint8_t> encode_wav(const AudioClip &clip) {
    require(clip.sample_rate_hz >= 1 && clip.sample_rate_hz <= 384000,
            "WAV sample rate must be in [1, 384000] Hz");
    require(!clip.pcm16.empty(), "Cannot encode an empty audio clip");
    const std::uint64_t data_bytes64 =
        static_cast<std::uint64_t>(clip.pcm16.size()) * sizeof(std::int16_t);
    require(data_bytes64 <= 0x7fff0000ULL, "Audio clip exceeds RIFF WAVE size limit");
    const auto data_bytes = static_cast<std::uint32_t>(data_bytes64);
    const std::uint32_t riff_size = 36U + data_bytes;
    std::vector<std::uint8_t> out;
    out.reserve(44U + data_bytes);
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    append_le32(out, riff_size);
    out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    append_le32(out, 16U);
    append_le16(out, 1U); // PCM format
    append_le16(out, 1U); // Mono
    append_le32(out, clip.sample_rate_hz);
    append_le32(out, clip.sample_rate_hz * 2U);
    append_le16(out, 2U);  // Block align
    append_le16(out, 16U); // Bits per sample
    out.insert(out.end(), {'d', 'a', 't', 'a'});
    append_le32(out, data_bytes);
    for (const std::int16_t sample : clip.pcm16)
        append_le16(out, static_cast<std::uint16_t>(sample));
    return out;
}

Spectrum compute_spectrum(const std::vector<double> &samples, double sample_rate_hz,
                          std::size_t max_bins) {
    require(std::isfinite(sample_rate_hz) && sample_rate_hz > 0.0,
            "Spectrum requires a finite positive sample rate");
    require(max_bins >= 4 && max_bins <= 65536, "Spectrum max_bins must be in [4, 65536]");
    std::vector<double> clean;
    clean.reserve(std::min<std::size_t>(samples.size(), 65536));
    double sum = 0.0;
    for (double v : samples) {
        if (std::isfinite(v)) {
            clean.push_back(v);
            sum += v;
            if (clean.size() == 65536)
                break;
        }
    }
    require(clean.size() >= 8, "Spectrum requires at least 8 finite samples in the window");
    const double mean = sum / static_cast<double>(clean.size());
    std::size_t fft_size = 8;
    while (fft_size * 2 <= clean.size() && fft_size < 65536)
        fft_size *= 2;
    if (fft_size < clean.size() && fft_size < 1024)
        fft_size *= 2;
    const std::size_t used = std::min(clean.size(), fft_size);
    std::vector<std::complex<double>> buf(fft_size, {0.0, 0.0});
    for (std::size_t i = 0; i < used; ++i) {
        const double window = used > 1
            ? 0.5 * (1.0 - std::cos(2.0 * pi * static_cast<double>(i) /
                                    static_cast<double>(used - 1)))
            : 1.0;
        buf[i] = {((clean[i] - mean) * window), 0.0};
    }
    fft_in_place(buf);

    const std::size_t half = fft_size / 2;
    const double scale = 4.0 / static_cast<double>(used); // Hann window amplitude compensation
    std::vector<SpectrumBin> raw_bins(half + 1);
    double dom_freq = 0.0;
    double dom_mag = 0.0;
    for (std::size_t k = 0; k <= half; ++k) {
        const double freq = static_cast<double>(k) * sample_rate_hz / static_cast<double>(fft_size);
        const double mag = std::abs(buf[k]) * (k == 0 || k == half ? scale * 0.5 : scale);
        raw_bins[k] = {freq, mag};
        if (k > 0 && mag > dom_mag) {
            dom_mag = mag;
            dom_freq = freq;
        }
    }

    Spectrum result;
    result.sample_rate_hz = sample_rate_hz;
    result.dominant_frequency_hz = dom_freq;
    result.dominant_magnitude = dom_mag;
    result.samples_used = used;
    if (raw_bins.size() <= max_bins) {
        result.bins = std::move(raw_bins);
    } else {
        result.bins.reserve(max_bins);
        for (std::size_t b = 0; b < max_bins; ++b) {
            const std::size_t begin = b * raw_bins.size() / max_bins;
            const std::size_t end = std::max(begin + 1, (b + 1) * raw_bins.size() / max_bins);
            SpectrumBin peak = raw_bins[begin];
            for (std::size_t k = begin + 1; k < end && k < raw_bins.size(); ++k) {
                if (raw_bins[k].magnitude > peak.magnitude)
                    peak = raw_bins[k];
            }
            result.bins.push_back(peak);
        }
    }
    return result;
}

namespace {
struct BiquadCoeffs {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a1 = 0.0, a2 = 0.0;
};

BiquadCoeffs make_butterworth_lowpass(double cutoff_hz, double sample_rate_hz) {
    const double nyquist = 0.5 * sample_rate_hz;
    const double fc = std::clamp(cutoff_hz, 0.5, nyquist * 0.98);
    const double k = std::tan(pi * fc / sample_rate_hz);
    const double k2 = k * k;
    const double sqrt2 = 1.4142135623730951;
    const double norm = 1.0 / (1.0 + sqrt2 * k + k2);
    BiquadCoeffs c;
    c.b0 = k2 * norm;
    c.b1 = 2.0 * c.b0;
    c.b2 = c.b0;
    c.a1 = 2.0 * (k2 - 1.0) * norm;
    c.a2 = (1.0 - sqrt2 * k + k2) * norm;
    return c;
}

BiquadCoeffs make_butterworth_highpass(double cutoff_hz, double sample_rate_hz) {
    const double nyquist = 0.5 * sample_rate_hz;
    const double fc = std::clamp(cutoff_hz, 0.5, nyquist * 0.98);
    const double k = std::tan(pi * fc / sample_rate_hz);
    const double k2 = k * k;
    const double sqrt2 = 1.4142135623730951;
    const double norm = 1.0 / (1.0 + sqrt2 * k + k2);
    BiquadCoeffs c;
    c.b0 = norm;
    c.b1 = -2.0 * c.b0;
    c.b2 = c.b0;
    c.a1 = 2.0 * (k2 - 1.0) * norm;
    c.a2 = (1.0 - sqrt2 * k + k2) * norm;
    return c;
}

void run_biquad_zero_phase(std::vector<double> &data, const BiquadCoeffs &c) {
    if (data.size() < 4)
        return;
    // Forward pass
    double z1 = 0.0, z2 = 0.0;
    for (double &x : data) {
        const double v = std::isfinite(x) ? x : 0.0;
        const double y = c.b0 * v + z1;
        z1 = c.b1 * v - c.a1 * y + z2;
        z2 = c.b2 * v - c.a2 * y;
        x = y;
    }
    // Backward pass for zero phase distortion
    z1 = 0.0;
    z2 = 0.0;
    for (std::size_t i = data.size(); i > 0; --i) {
        const double v = data[i - 1];
        const double y = c.b0 * v + z1;
        z1 = c.b1 * v - c.a1 * y + z2;
        z2 = c.b2 * v - c.a2 * y;
        data[i - 1] = y;
    }
}

std::vector<std::int16_t> wsola_stretch_pcm16(const std::vector<std::int16_t> &in,
                                              std::uint32_t sample_rate_hz,
                                              double speed) {
    if (in.empty() || std::abs(speed - 1.0) < 0.01)
        return in;
    const double clamped_speed = std::clamp(speed, 0.25, 4.0);
    const std::size_t out_len = std::max<std::size_t>(
        64, static_cast<std::size_t>(std::llround(static_cast<double>(in.size()) / clamped_speed)));
    const std::size_t win_len = std::clamp<std::size_t>(sample_rate_hz / 40U, 128U, 2048U); // ~25 ms
    const std::size_t hop_out = std::max<std::size_t>(16, win_len / 2);
    const std::size_t search_tol = std::max<std::size_t>(8, win_len / 6);

    if (in.size() <= win_len * 2) {
        return resample_pcm16(in, sample_rate_hz,
                              static_cast<std::uint32_t>(std::clamp(
                                  std::llround(sample_rate_hz / clamped_speed), 1000LL, 384000LL)));
    }

    std::vector<double> hann(win_len);
    for (std::size_t i = 0; i < win_len; ++i) {
        hann[i] = 0.5 * (1.0 - std::cos(2.0 * pi * static_cast<double>(i) /
                                        static_cast<double>(win_len - 1)));
    }

    std::vector<double> accum(out_len + win_len, 0.0);
    std::vector<double> norm(out_len + win_len, 0.0);

    std::size_t out_pos = 0;
    std::size_t prev_in_pos = 0;
    bool first = true;

    while (out_pos < out_len) {
        const double target_in_f = static_cast<double>(out_pos) * clamped_speed;
        const std::size_t nominal_in = static_cast<std::size_t>(std::min<double>(
            target_in_f, static_cast<double>(in.size() - win_len)));

        std::size_t best_in = nominal_in;
        if (!first && prev_in_pos + hop_out + win_len < in.size()) {
            const std::size_t expected = prev_in_pos + hop_out;
            const std::size_t lo = nominal_in > search_tol ? nominal_in - search_tol : 0;
            const std::size_t hi = std::min(in.size() - win_len, nominal_in + search_tol);
            double best_corr = -1e30;
            const std::size_t cmp_len = std::min<std::size_t>(hop_out, 128);
            for (std::size_t cand = lo; cand <= hi; cand += 2) {
                double corr = 0.0;
                for (std::size_t k = 0; k < cmp_len; ++k) {
                    corr += static_cast<double>(in[expected + k]) *
                            static_cast<double>(in[cand + k]);
                }
                if (corr > best_corr) {
                    best_corr = corr;
                    best_in = cand;
                }
            }
        }
        first = false;
        prev_in_pos = best_in;

        for (std::size_t i = 0; i < win_len && (out_pos + i) < accum.size(); ++i) {
            const double w = hann[i];
            accum[out_pos + i] += static_cast<double>(in[best_in + i]) * w;
            norm[out_pos + i] += w;
        }
        out_pos += hop_out;
    }

    std::vector<std::int16_t> out(out_len, 0);
    for (std::size_t i = 0; i < out_len; ++i) {
        const double val = norm[i] > 1e-6 ? (accum[i] / norm[i]) : 0.0;
        out[i] = static_cast<std::int16_t>(std::clamp(std::lround(val), -32768L, 32767L));
    }
    return out;
}
} // namespace

std::vector<double> filter_bandpass(const std::vector<double> &samples, double sample_rate_hz,
                                    double low_cut_hz, double high_cut_hz) {
    require(std::isfinite(sample_rate_hz) && sample_rate_hz > 0.0,
            "Filter requires a finite positive sample rate");
    std::vector<double> result = samples;
    if (result.size() < 4)
        return result;
    const double nyquist = 0.5 * sample_rate_hz;
    if (std::isfinite(low_cut_hz) && low_cut_hz > 0.5 && low_cut_hz < nyquist * 0.98) {
        run_biquad_zero_phase(result, make_butterworth_highpass(low_cut_hz, sample_rate_hz));
    }
    if (std::isfinite(high_cut_hz) && high_cut_hz > 0.5 && high_cut_hz < nyquist * 0.98) {
        if (low_cut_hz <= 0.0 || high_cut_hz > low_cut_hz) {
            run_biquad_zero_phase(result, make_butterworth_lowpass(high_cut_hz, sample_rate_hz));
        }
    }
    return result;
}

Spectrogram compute_spectrogram(const std::vector<double> &samples, double sample_rate_hz,
                                Range range, std::size_t time_bins, std::size_t freq_bins) {
    require(std::isfinite(sample_rate_hz) && sample_rate_hz > 0.0,
            "Spectrogram requires a finite positive sample rate");
    require(time_bins >= 4 && time_bins <= 512, "Spectrogram time_bins must be in [4, 512]");
    require(freq_bins >= 4 && freq_bins <= 512, "Spectrogram freq_bins must be in [4, 512]");

    std::vector<double> clean;
    clean.reserve(std::min<std::size_t>(samples.size(), 131072));
    double sum = 0.0;
    for (double v : samples) {
        if (std::isfinite(v)) {
            clean.push_back(v);
            sum += v;
            if (clean.size() == 131072)
                break;
        }
    }
    require(clean.size() >= 16, "Spectrogram requires at least 16 finite samples");
    const double mean = sum / static_cast<double>(clean.size());
    for (double &v : clean)
        v -= mean;

    std::size_t fft_size = 64;
    while (fft_size < freq_bins * 2 && fft_size < 1024)
        fft_size *= 2;
    while (fft_size > clean.size() && fft_size > 16)
        fft_size /= 2;
    const std::size_t half = fft_size / 2;

    std::vector<double> hann(fft_size);
    for (std::size_t i = 0; i < fft_size; ++i) {
        hann[i] = 0.5 * (1.0 - std::cos(2.0 * pi * static_cast<double>(i) /
                                        static_cast<double>(fft_size - 1)));
    }

    std::vector<double> raw_matrix(time_bins * freq_bins, 0.0);
    double max_mag = 1e-12;
    double dom_freq = 0.0;
    double dom_time_ratio = 0.0;

    std::vector<std::complex<double>> buf(fft_size);
    for (std::size_t t = 0; t < time_bins; ++t) {
        const double ratio = time_bins > 1
            ? static_cast<double>(t) / static_cast<double>(time_bins - 1)
            : 0.0;
        const std::size_t max_start = clean.size() > fft_size ? (clean.size() - fft_size) : 0;
        const std::size_t start = static_cast<std::size_t>(std::llround(ratio * static_cast<double>(max_start)));

        for (std::size_t i = 0; i < fft_size; ++i) {
            const double s = (start + i < clean.size()) ? clean[start + i] : 0.0;
            buf[i] = {s * hann[i], 0.0};
        }
        fft_in_place(buf);

        const double scale = 4.0 / static_cast<double>(fft_size);
        for (std::size_t f = 0; f < freq_bins; ++f) {
            const std::size_t k_begin = std::max<std::size_t>(1, f * half / freq_bins);
            const std::size_t k_end = std::max(k_begin + 1, (f + 1) * (half + 1) / freq_bins);
            double peak = 0.0;
            std::size_t peak_k = k_begin;
            for (std::size_t k = k_begin; k < k_end && k <= half; ++k) {
                const double mag = std::abs(buf[k]) * scale;
                if (mag > peak) {
                    peak = mag;
                    peak_k = k;
                }
            }
            raw_matrix[t * freq_bins + f] = peak;
            if (peak > max_mag) {
                max_mag = peak;
                dom_freq = static_cast<double>(peak_k) * sample_rate_hz / static_cast<double>(fft_size);
                dom_time_ratio = ratio;
            }
        }
    }

    Spectrogram out;
    out.range = range;
    out.sample_rate_hz = sample_rate_hz;
    out.max_frequency_hz = 0.5 * sample_rate_hz;
    out.time_bins = time_bins;
    out.freq_bins = freq_bins;
    out.min_db = -72.0;
    out.max_db = 0.0;
    out.dominant_frequency_hz = dom_freq;
    out.dominant_time_ratio = dom_time_ratio;
    out.samples_used = clean.size();
    out.magnitudes_db.resize(time_bins * freq_bins);

    for (std::size_t i = 0; i < raw_matrix.size(); ++i) {
        const double norm = std::max(1e-9, raw_matrix[i] / max_mag);
        const double db = 20.0 * std::log10(norm);
        out.magnitudes_db[i] = std::clamp(db, out.min_db, out.max_db);
    }
    return out;
}

struct AudioPlayer::Impl {
    mutable std::mutex mutex;
    std::condition_variable cv;
    AudioClip clip;
    std::vector<std::int16_t> base_pcm;
    std::vector<std::int16_t> playback_pcm;
    std::uint32_t playback_rate = 16000;
    std::atomic<std::size_t> cursor_sample{0};
    std::atomic<double> volume{0.85};
    std::atomic<double> speed{1.0};
    std::atomic<bool> muted{false};
    std::atomic<bool> playing{false};
    std::atomic<bool> stop_worker{false};
    std::string backend = "software-null";
    std::thread worker;

    ~Impl() { stop_thread(); }

    void rebuild_stretched_pcm() {
        if (base_pcm.empty()) {
            playback_pcm.clear();
            return;
        }
        const double sp = std::clamp(speed.load(std::memory_order_relaxed), 0.25, 4.0);
        if (std::abs(sp - 1.0) < 0.01) {
            playback_pcm = base_pcm;
        } else {
            playback_pcm = wsola_stretch_pcm16(base_pcm, playback_rate, sp);
        }
    }

    void stop_thread() {
        stop_worker.store(true, std::memory_order_relaxed);
        playing.store(false, std::memory_order_relaxed);
        cv.notify_all();
        if (worker.joinable())
            worker.join();
    }

    void start_thread() {
        stop_thread();
        if (playback_pcm.empty())
            return;
        if (cursor_sample.load(std::memory_order_relaxed) >= playback_pcm.size())
            cursor_sample.store(0, std::memory_order_relaxed);
        stop_worker.store(false, std::memory_order_relaxed);
        playing.store(true, std::memory_order_relaxed);
        worker = std::thread([this] { run_loop(); });
    }

    void run_loop() {
        const std::uint32_t rate = std::max<std::uint32_t>(8000U, playback_rate);
        const std::size_t chunk_frames = std::max<std::size_t>(rate / 50U, 160U); // ~20ms
        std::vector<std::int16_t> chunk(chunk_frames, 0);

        const bool force_null = prefer_null_audio();
#ifdef _WIN32
        std::unique_ptr<WinMmSink> win_sink;
        if (!force_null) {
            win_sink = std::make_unique<WinMmSink>(rate);
            if (win_sink->is_open()) {
                std::lock_guard<std::mutex> lock(mutex);
                backend = "winmm";
            } else {
                win_sink.reset();
            }
        }
#elif defined(__linux__)
        std::unique_ptr<PulseSink> pulse_sink;
        std::unique_ptr<AlsaSink> alsa_sink;
        if (!force_null) {
            pulse_sink = std::make_unique<PulseSink>(rate);
            if (pulse_sink->is_open()) {
                std::lock_guard<std::mutex> lock(mutex);
                backend = "pulseaudio";
            } else {
                pulse_sink.reset();
                alsa_sink = std::make_unique<AlsaSink>(rate);
                if (alsa_sink->is_open()) {
                    std::lock_guard<std::mutex> lock(mutex);
                    backend = "alsa";
                } else {
                    alsa_sink.reset();
                }
            }
        }
#endif
        while (!stop_worker.load(std::memory_order_relaxed)) {
            const std::size_t pos = cursor_sample.load(std::memory_order_relaxed);
            if (pos >= playback_pcm.size()) {
                playing.store(false, std::memory_order_relaxed);
                break;
            }
            const std::size_t count = std::min(chunk_frames, playback_pcm.size() - pos);
            const double gain = muted.load(std::memory_order_relaxed)
                ? 0.0
                : std::clamp(volume.load(std::memory_order_relaxed), 0.0, 1.0);
            for (std::size_t i = 0; i < count; ++i) {
                const double scaled = static_cast<double>(playback_pcm[pos + i]) * gain;
                chunk[i] = static_cast<std::int16_t>(
                    std::clamp(std::lround(scaled), -32768L, 32767L));
            }
            cursor_sample.store(pos + count, std::memory_order_relaxed);

            bool wrote_hw = false;
#ifdef _WIN32
            if (win_sink)
                wrote_hw = win_sink->write(chunk.data(), count, stop_worker);
#elif defined(__linux__)
            if (pulse_sink)
                wrote_hw = pulse_sink->write(chunk.data(), count);
            else if (alsa_sink)
                wrote_hw = alsa_sink->write(chunk.data(), count);
#endif
            if (!wrote_hw) {
                const auto sleep_us = static_cast<std::int64_t>(
                    (static_cast<std::uint64_t>(count) * 1000000ULL) / rate);
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock, std::chrono::microseconds(std::max<std::int64_t>(1000, sleep_us)),
                            [this] { return stop_worker.load(std::memory_order_relaxed); });
            }
        }
#ifdef _WIN32
        if (win_sink && stop_worker.load(std::memory_order_relaxed))
            win_sink->drop();
#elif defined(__linux__)
        if (pulse_sink && stop_worker.load(std::memory_order_relaxed))
            pulse_sink->drop();
        if (alsa_sink && stop_worker.load(std::memory_order_relaxed))
            alsa_sink->drop();
#endif
    }
};

AudioPlayer::AudioPlayer() : impl_(std::make_unique<Impl>()) {}
AudioPlayer::~AudioPlayer() = default;

void AudioPlayer::load(AudioClip clip) {
    impl_->stop_thread();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->clip = std::move(clip);
    if (impl_->clip.pcm16.empty() || impl_->clip.sample_rate_hz == 0) {
        impl_->base_pcm.clear();
        impl_->playback_pcm.clear();
        impl_->playback_rate = 16000;
    } else {
        impl_->playback_rate = std::clamp<std::uint32_t>(impl_->clip.sample_rate_hz, 16000U, 96000U);
        impl_->base_pcm = resample_pcm16(
            impl_->clip.pcm16, impl_->clip.sample_rate_hz, impl_->playback_rate);
        impl_->rebuild_stretched_pcm();
    }
    impl_->cursor_sample.store(0, std::memory_order_relaxed);
}

void AudioPlayer::clear() {
    impl_->stop_thread();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->clip = {};
    impl_->base_pcm.clear();
    impl_->playback_pcm.clear();
    impl_->cursor_sample.store(0, std::memory_order_relaxed);
}

void AudioPlayer::play() {
    impl_->start_thread();
}

void AudioPlayer::pause() {
    impl_->stop_thread();
}

void AudioPlayer::stop() {
    impl_->stop_thread();
    impl_->cursor_sample.store(0, std::memory_order_relaxed);
}

void AudioPlayer::seek(Time timestamp_us) {
    const bool was_playing = impl_->playing.load(std::memory_order_relaxed);
    impl_->stop_thread();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto &r = impl_->clip.range;
        if (impl_->playback_pcm.empty() || r.end <= r.start || timestamp_us <= r.start) {
            impl_->cursor_sample.store(0, std::memory_order_relaxed);
        } else if (timestamp_us >= r.end) {
            impl_->cursor_sample.store(impl_->playback_pcm.size(), std::memory_order_relaxed);
        } else {
            const long double fraction =
                static_cast<long double>(timestamp_us - r.start) /
                static_cast<long double>(r.end - r.start);
            const auto index = static_cast<std::size_t>(std::clamp<long double>(
                fraction * static_cast<long double>(impl_->playback_pcm.size()),
                0.0L, static_cast<long double>(impl_->playback_pcm.size() - 1)));
            impl_->cursor_sample.store(index, std::memory_order_relaxed);
        }
    }
    if (was_playing)
        impl_->start_thread();
}

void AudioPlayer::set_volume(double volume) {
    require(std::isfinite(volume) && volume >= 0.0 && volume <= 1.0,
            "Audio volume must be in [0.0, 1.0]");
    impl_->volume.store(volume, std::memory_order_relaxed);
}

double AudioPlayer::volume() const noexcept {
    return impl_->volume.load(std::memory_order_relaxed);
}

void AudioPlayer::set_playback_speed(double speed) {
    require(std::isfinite(speed) && speed >= 0.01 && speed <= 1000.0,
            "Audio playback speed must be in [0.01, 1000.0]");
    const double clamped = std::clamp(speed, 0.25, 4.0);
    if (std::abs(impl_->speed.load(std::memory_order_relaxed) - clamped) < 0.01)
        return;
    const bool was_playing = impl_->playing.load(std::memory_order_relaxed);
    const Time cur_pos = position();
    impl_->stop_thread();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->speed.store(clamped, std::memory_order_relaxed);
        impl_->rebuild_stretched_pcm();
    }
    seek(cur_pos);
    if (was_playing)
        impl_->start_thread();
}

double AudioPlayer::playback_speed() const noexcept {
    return impl_->speed.load(std::memory_order_relaxed);
}

void AudioPlayer::set_muted(bool muted) noexcept {
    impl_->muted.store(muted, std::memory_order_relaxed);
}

bool AudioPlayer::muted() const noexcept {
    return impl_->muted.load(std::memory_order_relaxed);
}

bool AudioPlayer::is_playing() const noexcept {
    return impl_->playing.load(std::memory_order_relaxed);
}

bool AudioPlayer::has_clip() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return !impl_->playback_pcm.empty();
}

Time AudioPlayer::position() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto &r = impl_->clip.range;
    if (impl_->playback_pcm.empty() || r.end <= r.start)
        return r.start;
    const std::size_t cur = std::min(
        impl_->cursor_sample.load(std::memory_order_relaxed), impl_->playback_pcm.size());
    const long double fraction =
        static_cast<long double>(cur) / static_cast<long double>(impl_->playback_pcm.size());
    const auto offset = static_cast<Time>(fraction * static_cast<long double>(r.end - r.start));
    return std::clamp(r.start + offset, r.start, r.end);
}

std::uint32_t AudioPlayer::sample_rate_hz() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->clip.sample_rate_hz;
}

std::string AudioPlayer::backend_name() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->backend;
}
} // namespace edgeecho

