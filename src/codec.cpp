#include "archive_support.hpp"
#include "signal_decode.hpp"

#include <array>
#include <cmath>
#include <sstream>
#include <unordered_set>

namespace edgeecho {
namespace {
namespace replay = detail::replay;

struct Feature {
    const char *id;
    const char *label;
    double scale;
    const char *physical_unit;
    const char *description_zh;
};

// Presentation order, labels and physical calibration for each sensor family.
constexpr std::array<Feature, 6> xyzs_frequency{{
    {"x_high_freq_acc_rms", "X 高频加速度有效值 / X HF Acc RMS", 0.0980665, "m/s²", "X轴高频加速度有效值 (0.0980665 m/s²/count)"},
    {"x_low_freq_velocity_rms", "X 低频速度有效值 / X LF Vel RMS", 0.01, "mm/s", "X轴低频速度有效值 (0.01 mm/s/count)"},
    {"y_high_freq_acc_rms", "Y 高频加速度有效值 / Y HF Acc RMS", 0.0980665, "m/s²", "Y轴高频加速度有效值 (0.0980665 m/s²/count)"},
    {"y_low_freq_velocity_rms", "Y 低频速度有效值 / Y LF Vel RMS", 0.01, "mm/s", "Y轴低频速度有效值 (0.01 mm/s/count)"},
    {"z_high_freq_acc_rms", "Z 高频加速度有效值 / Z HF Acc RMS", 0.0980665, "m/s²", "Z轴高频加速度有效值 (0.0980665 m/s²/count)"},
    {"z_low_freq_velocity_rms", "Z 低频速度有效值 / Z LF Vel RMS", 0.01, "mm/s", "Z轴低频速度有效值 (0.01 mm/s/count)"},
}};
constexpr std::array<Feature, 16> common_features{{
    {"temperature", "温度 / Temperature", 0.01, "°C", "传感器温度 (0.01 °C/count = 10 m°C)"},
    {"x_acc_peak_to_peak", "X 加速度峰峰值 / X Acc Peak-to-Peak", 0.0980665, "m/s²", "X轴加速度峰峰值 (0.0980665 m/s²/count)"},
    {"y_acc_peak_to_peak", "Y 加速度峰峰值 / Y Acc Peak-to-Peak", 0.0980665, "m/s²", "Y轴加速度峰峰值 (0.0980665 m/s²/count)"},
    {"z_acc_peak_to_peak", "Z 加速度峰峰值 / Z Acc Peak-to-Peak", 0.0980665, "m/s²", "Z轴加速度峰峰值 (0.0980665 m/s²/count)"},
    {"x_acc_peak", "X 加速度峰值 / X Acc Peak", 0.0980665, "m/s²", "X轴加速度峰值 (0.0980665 m/s²/count)"},
    {"y_acc_peak", "Y 加速度峰值 / Y Acc Peak", 0.0980665, "m/s²", "Y轴加速度峰值 (0.0980665 m/s²/count)"},
    {"z_acc_peak", "Z 加速度峰值 / Z Acc Peak", 0.0980665, "m/s²", "Z轴加速度峰值 (0.0980665 m/s²/count)"},
    {"x_acc_rms", "X 加速度有效值 / X Acc RMS", 0.0980665, "m/s²", "X轴加速度有效值 (0.0980665 m/s²/count)"},
    {"y_acc_rms", "Y 加速度有效值 / Y Acc RMS", 0.0980665, "m/s²", "Y轴加速度有效值 (0.0980665 m/s²/count)"},
    {"z_acc_rms", "Z 加速度有效值 / Z Acc RMS", 0.0980665, "m/s²", "Z轴加速度有效值 (0.0980665 m/s²/count)"},
    {"x_kurtosis", "X 峭度指标 / X Kurtosis", 0.01, "index", "X轴峭度无量纲指标 (0.01 /count)"},
    {"y_kurtosis", "Y 峭度指标 / Y Kurtosis", 0.01, "index", "Y轴峭度无量纲指标 (0.01 /count)"},
    {"z_kurtosis", "Z 峭度指标 / Z Kurtosis", 0.01, "index", "Z轴峭度无量纲指标 (0.01 /count)"},
    {"x_velocity_rms", "X 速度有效值 / X Velocity RMS", 0.01, "mm/s", "X轴速度有效值 (0.01 mm/s/count)"},
    {"y_velocity_rms", "Y 速度有效值 / Y Velocity RMS", 0.01, "mm/s", "Y轴速度有效值 (0.01 mm/s/count)"},
    {"z_velocity_rms", "Z 速度有效值 / Z Velocity RMS", 0.01, "mm/s", "Z轴速度有效值 (0.01 mm/s/count)"},
}};
constexpr std::array<Feature, 7> xyzs_sound{{
    {"sound_rms_db", "声音有效值 / Sound RMS", 0.01, "dB", "声音有效值分贝 (0.01 dB/count)"},
    {"sound_peak_db", "声音峰值 / Sound Peak", 0.01, "dB", "声音峰值分贝 (0.01 dB/count)"},
    {"sound_peak_to_peak_db", "声音峰峰值 / Sound Peak-to-Peak", 0.01, "dB", "声音峰峰值分贝 (0.01 dB/count)"},
    {"zero_crossing_rate_percent", "过零率 / Zero-Crossing Rate", 1.0, "%", "声学信号过零率百分比 (1 %/count)"},
    {"spectral_centroid_hz", "频谱重心 / Spectral Centroid", 0.1, "Hz", "声学频谱重心频率 (0.1 Hz/count)"},
    {"spectral_flux", "频谱变化指标 / Spectral Flux", 0.01, "index", "频谱通量变化无量纲指标 (0.01 /count)"},
    {"startup_flags", "XYZ 运行判定标志 / Startup Flags", 1.0, "mask", "XYZ三轴运行判定位掩码 (bit0/1/2 = X/Y/Z)"},
}};

constexpr std::array<Feature, 3> xyz_displacement{{
    {"x_displacement_peak_to_peak", "X 位移峰峰值 / X Displacement Peak-to-Peak", 1e-6, "m", "X轴位移峰峰值 (1 μm/count)"},
    {"y_displacement_peak_to_peak", "Y 位移峰峰值 / Y Displacement Peak-to-Peak", 1e-6, "m", "Y轴位移峰峰值 (1 μm/count)"},
    {"z_displacement_peak_to_peak", "Z 位移峰峰值 / Z Displacement Peak-to-Peak", 1e-6, "m", "Z轴位移峰峰值 (1 μm/count)"},
}};

template<class Visit>
void visit_features(const std::string &encoding, Visit visit) {
    const auto visit_group = [&](const auto &group) {
        for (const auto &feature : group) visit(feature);
    };
    if (encoding == "iio-le-72-v1") {
        visit_group(xyzs_frequency);
        visit_group(common_features);
        visit_group(xyzs_sound);
    } else if (encoding == "iio-le-48-v1") {
        visit_group(common_features);
        visit_group(xyz_displacement);
    }
}

void check_cancel(const Cancel &cancel) {
    if (cancel && cancel->load(std::memory_order_relaxed))
        throw Error("query cancelled");
}

bool compact_waveform(const std::string &encoding) noexcept {
    return encoding == "s16le" || encoding == "s32le" || encoding == "f32le" || encoding == "f64le";
}

void emit_points(const replay::SignalView &signal, const std::string &channel,
                 std::size_t first, std::size_t last, const std::function<void(Point)> &emit,
                 const Cancel &cancel) {
    const auto selected = signal.find_channel(channel);
    if (!selected) throw Error("unknown channel: " + channel);
    for (std::size_t sample = first; sample < last; ++sample) {
        check_cancel(cancel);
        const auto value = signal.value(sample, *selected);
        emit({signal.timestamp(sample), value.value_or(0), !value});
    }
}

void validate_utf8(const std::string &text) {
    for (std::size_t pos = 0; pos < text.size();) {
        const auto first = static_cast<unsigned char>(text[pos++]);
        if (first == 0)
            throw Error("metadata contains NUL");
        if (first < 0x80)
            continue;
        const unsigned trailing = first >= 0xc2 && first <= 0xdf ? 1U
            : first >= 0xe0 && first <= 0xef ? 2U
            : first >= 0xf0 && first <= 0xf4 ? 3U : 0U;
        if (!trailing || trailing > text.size() - pos)
            throw Error("invalid metadata UTF-8");
        const auto next = static_cast<unsigned char>(text[pos]);
        if ((first == 0xe0 && next < 0xa0) || (first == 0xed && next >= 0xa0) ||
            (first == 0xf0 && next < 0x90) || (first == 0xf4 && next >= 0x90))
            throw Error("invalid metadata UTF-8 code point");
        for (unsigned i = 0; i < trailing; ++i) {
            const auto ch = static_cast<unsigned char>(text[pos++]);
            if (ch < 0x80 || ch > 0xbf)
                throw Error("invalid metadata UTF-8 continuation");
        }
    }
}
} // namespace

std::vector<Channel> channels(const std::string &encoding, int kind) {
    if (kind == 0 || kind == 1)
        return {{"value", "Value"}};
    if (kind != 2)
        return {};
    if (encoding == "iio-le-24-v1")
        return {{"x", "X (raw)"}, {"y", "Y (raw)"}, {"z", "Z (raw)"}};
    if (encoding == "iio-le-16-v1")
        return {{"sound", "Sound PCM (raw)"}};
    if (compact_waveform(encoding))
        return {{"value", "Value"}};
    std::vector<Channel> result;
    visit_features(encoding, [&](const Feature &feature) {
        result.push_back({feature.id, feature.label});
    });
    return result;
}

bool is_waveform_encoding(const std::string &encoding) noexcept {
    return encoding == "iio-le-24-v1" || encoding == "iio-le-16-v1" || compact_waveform(encoding);
}

ChannelCalibration default_calibration(const std::string &encoding, const std::string &channel) {
    std::optional<ChannelCalibration> calibration;
    visit_features(encoding, [&](const Feature &feature) {
        if (feature.id == channel)
            calibration = ChannelCalibration{feature.scale, 0.0, feature.physical_unit,
                                             feature.description_zh, true};
    });
    if (calibration) return *calibration;
    if (encoding == "iio-le-24-v1" &&
        (channel == "x" || channel == "y" || channel == "z" || channel == "xyz")) {
        // Standard ±16g 16-bit MEMS accelerometer sensitivity: 16 * 9.80665 / 32768 m/s²/count
        return {0.0047884033203125, 0.0, "m/s²",
                "MEMS 三轴原始振动加速度 (±16g 量程, 0.004788 m/s²/count)", true};
    }
    if (encoding == "iio-le-16-v1" && channel == "sound") {
        // Normalized 24/32-bit MEMS acoustic full-scale pressure reference (Pa equivalent)
        return {1.0 / 1048576.0, 0.0, "Pa (归一化声压)",
                "MEMS 原始声学 PCM 采样点 (归一化声压比)", true};
    }
    if (channel == "value") {
        return {0.0980665, 0.0, "m/s²", "标量特征物理换算 (0.0980665 m/s²/count)", true};
    }
    return {1.0, 0.0, "count", "ADC 原始计数", false};
}

double apply_calibration(double raw_value, const ChannelCalibration &cal) noexcept {
    if (!std::isfinite(raw_value))
        return 0.0;
    return raw_value * cal.scale + cal.offset;
}

void detail::decode_window(const replay::SignalView &signal, const std::string &channel, Range range,
                           const std::function<void(Point)> &emit, const Cancel &cancel) {
    const auto [first, last] = signal.sample_range({range.start, range.end});
    emit_points(signal, channel, first, last, emit, cancel);
}

void decode(const Record &record, const std::string &channel,
            const std::function<void(Point)> &emit, const Cancel &cancel) {
    check_cancel(cancel);
    if (!emit) throw Error("decode requires an output callback");

    // Raw detail decodes stored values even when history views mask their quality.
    replay::Record raw;
    raw.id = record.id;
    raw.timestamp = record.timestamp;
    raw.duration_us = record.duration;
    raw.sample_rate_hz = record.rate;
    raw.encoding = record.encoding;
    switch (record.kind) {
        case 0:
            raw.quality = replay::missing;
            break;
        case 1:
            if (!record.scalar) throw Error("scalar record has no value");
            raw.value = *record.scalar;
            break;
        case 2:
            raw.value = record.block;
            break;
        default:
            throw Error("unsupported record kind");
    }
    try {
        const replay::SignalView signal(raw);
        emit_points(signal, channel, 0, signal.sample_count(), emit, cancel);
    } catch (const replay::Error &error) {
        detail::throw_replay_error(error);
    }
}

std::vector<std::pair<std::string, std::string>> unpack_metadata(const std::string &data) {
    if (data.size() > 65536)
        throw Error("metadata exceeds 65536 bytes");
    std::size_t pos = 0;
    const auto part = [&]() {
        const auto colon = data.find(':', pos);
        if (colon == std::string::npos || colon == pos || colon - pos > 8)
            throw Error("invalid metadata length prefix");
        std::size_t length = 0;
        while (pos < colon) {
            const char digit = data[pos++];
            if (digit < '0' || digit > '9')
                throw Error("invalid metadata length digit");
            length = length * 10 + static_cast<std::size_t>(digit - '0');
        }
        ++pos;
        if (length > data.size() - pos)
            throw Error("truncated metadata value");
        auto result = data.substr(pos, length);
        pos += length;
        validate_utf8(result);
        return result;
    };
    std::vector<std::pair<std::string, std::string>> result;
    std::unordered_set<std::string> keys;
    while (pos < data.size()) {
        auto key = part();
        auto value = part();
        if (key.empty() || key.size() > 512 || value.size() > 4096)
            throw Error("empty or oversized metadata key/value");
        if (!keys.emplace(key).second)
            throw Error("duplicate metadata key");
        if (result.size() >= 128)
            throw Error("too many metadata entries");
        result.emplace_back(std::move(key), std::move(value));
    }
    return result;
}

std::string quality_text(std::uint32_t quality) {
    if (quality == 0)
        return "good";
    std::string result;
    const auto add = [&](const std::string &label) {
        if (!result.empty())
            result += " | ";
        result += label;
    };
    if (quality & 1U) add("invalid");
    if (quality & 2U) add("missing");
    if (quality & 4U) add("uncertain_time");
    if (quality & 8U) add("partial");
    if (quality & ~15U) {
        std::ostringstream text;
        text << "unknown(0x" << std::hex << (quality & ~15U) << ')';
        add(text.str());
    }
    return result;
}
} // namespace edgeecho
