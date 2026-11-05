#include "edgeecho/core.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace edgeecho {
namespace {
struct Feature {
    const char *id;
    const char *label;
};

// Public XYZS IIO scan order. Values remain ADC/feature counts: the stored
// blocks do not contain the scale configuration needed for physical units.
constexpr std::array<Feature, 29> features{{
    {"x_high_freq_acc_rms", "X high-frequency acceleration RMS (raw)"},
    {"x_low_freq_velocity_rms", "X low-frequency velocity RMS (raw)"},
    {"y_high_freq_acc_rms", "Y high-frequency acceleration RMS (raw)"},
    {"y_low_freq_velocity_rms", "Y low-frequency velocity RMS (raw)"},
    {"z_high_freq_acc_rms", "Z high-frequency acceleration RMS (raw)"},
    {"z_low_freq_velocity_rms", "Z low-frequency velocity RMS (raw)"},
    {"temperature", "Temperature (raw)"},
    {"x_acc_peak_to_peak", "X acceleration peak-to-peak (raw)"},
    {"y_acc_peak_to_peak", "Y acceleration peak-to-peak (raw)"},
    {"z_acc_peak_to_peak", "Z acceleration peak-to-peak (raw)"},
    {"x_acc_peak", "X acceleration peak (raw)"},
    {"y_acc_peak", "Y acceleration peak (raw)"},
    {"z_acc_peak", "Z acceleration peak (raw)"},
    {"x_acc_rms", "X acceleration RMS (raw)"},
    {"y_acc_rms", "Y acceleration RMS (raw)"},
    {"z_acc_rms", "Z acceleration RMS (raw)"},
    {"x_kurtosis", "X kurtosis (raw)"},
    {"y_kurtosis", "Y kurtosis (raw)"},
    {"z_kurtosis", "Z kurtosis (raw)"},
    {"x_velocity_rms", "X velocity RMS (raw)"},
    {"y_velocity_rms", "Y velocity RMS (raw)"},
    {"z_velocity_rms", "Z velocity RMS (raw)"},
    {"sound_rms_db", "Sound RMS (raw)"},
    {"sound_peak_db", "Sound peak (raw)"},
    {"sound_peak_to_peak_db", "Sound peak-to-peak (raw)"},
    {"zero_crossing_rate_percent", "Zero-crossing rate (raw)"},
    {"spectral_centroid_hz", "Spectral centroid (raw)"},
    {"spectral_flux", "Spectral flux (raw)"},
    {"startup_flags", "Startup flags"},
}};

void check_cancel(const Cancel &cancel) {
    if (cancel && cancel->load(std::memory_order_relaxed))
        throw Error("query cancelled");
}

std::uint64_t little_unsigned(const std::uint8_t *bytes, std::size_t width) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i)
        value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
    return value;
}

double signed_value(const std::uint8_t *bytes, std::size_t width) {
    const auto value = little_unsigned(bytes, width);
    const auto sign = std::uint64_t{1} << (width * 8 - 1);
    if (value & sign)
        return static_cast<double>(static_cast<std::int64_t>(value) -
                                   static_cast<std::int64_t>(sign * 2));
    return static_cast<double>(value);
}

double float_value(const std::uint8_t *bytes, std::size_t width) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                  "f32le requires IEEE 754 binary32");
    static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559,
                  "f64le requires IEEE 754 binary64");
    if (width == 4) {
        const auto bits = static_cast<std::uint32_t>(little_unsigned(bytes, width));
        float value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return static_cast<double>(value);
    }
    const auto bits = little_unsigned(bytes, width);
    double value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
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
    if (encoding == "iio-le-72-v1") {
        std::vector<Channel> result;
        result.reserve(features.size());
        for (const auto &feature : features)
            result.push_back({feature.id, feature.label});
        return result;
    }
    if (encoding == "s16le" || encoding == "s32le" || encoding == "f32le" ||
        encoding == "f64le")
        return {{"value", "Value"}};
    return {};
}

void decode(const Record &record, const std::string &channel,
            const std::function<void(Point)> &emit, const Cancel &cancel) {
    check_cancel(cancel);
    if (!emit)
        throw Error("decode requires an output callback");
    if (record.timestamp < 0 || record.timestamp >= max_time || record.duration < 0 ||
        record.duration > max_time - record.timestamp)
        throw Error("record timestamp/duration is out of range");
    const auto available = channels(record.encoding, record.kind);
    std::size_t selected = 0;
    while (selected < available.size() && available[selected].id != channel)
        ++selected;
    if (selected == available.size())
        throw Error(available.empty() ? "unsupported record encoding/kind: " + record.encoding
                                      : "unknown channel: " + channel);

    if (record.kind == 0) {
        emit({record.timestamp, 0, true});
        return;
    }
    if (record.kind == 1) {
        if (!record.scalar)
            throw Error("scalar record has no value");
        const auto value = *record.scalar;
        emit({record.timestamp, std::isfinite(value) ? value : 0, !std::isfinite(value)});
        return;
    }

    const bool feature_scan = record.encoding == "iio-le-72-v1";
    const bool float_samples = record.encoding == "f32le" || record.encoding == "f64le";
    std::size_t stride = 0, offset = 0, width = 0;
    if (record.encoding == "iio-le-24-v1") {
        stride = 24; offset = selected * 2; width = 2;
    } else if (record.encoding == "iio-le-16-v1") {
        stride = 16; width = 4;
    } else if (feature_scan) {
        stride = 72; offset = selected * 2; width = selected == 28 ? 1 : 2;
    } else {
        width = record.encoding == "s16le" ? 2 : record.encoding == "f64le" ? 8 : 4;
        stride = width;
    }
    if (record.block.empty() || record.block.size() % stride != 0)
        throw Error("empty or truncated block for encoding: " + record.encoding);
    if (record.duration <= 0)
        throw Error("block requires a positive duration");
    const auto count = record.block.size() / stride;
    long double period = 0;
    if (!feature_scan) {
        if (!record.rate || !std::isfinite(*record.rate) || *record.rate <= 0)
            throw Error("waveform requires a finite positive sample rate");
        period = 1000000.0L / static_cast<long double>(*record.rate);
        const auto last_offset = std::floor(static_cast<long double>(count - 1) * period);
        if (!std::isfinite(last_offset) || last_offset < 0 ||
            last_offset >= static_cast<long double>(record.duration))
            throw Error("waveform sample timestamps exceed its half-open duration");
    }
    for (std::size_t i = 0; i < count; ++i) {
        check_cancel(cancel);
        const auto *bytes = record.block.data() + i * stride + offset;
        double value = 0;
        if (feature_scan && selected != 6)
            value = static_cast<double>(little_unsigned(bytes, width));
        else if (float_samples)
            value = float_value(bytes, width);
        else
            value = signed_value(bytes, width);
        // Preflight bounds above ensure the cast and addition cannot overflow.
        // Feature values share the record anchor; they have no uniform rate.
        const Time timestamp = feature_scan ? record.timestamp : record.timestamp +
            static_cast<Time>(std::floor(static_cast<long double>(i) * period));
        emit({timestamp, std::isfinite(value) ? value : 0, !std::isfinite(value)});
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
