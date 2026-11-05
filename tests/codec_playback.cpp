#include "edgeecho/core.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace {
using namespace edgeecho;

void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

template<class F> void rejects(F run, const std::string &message) {
    bool rejected = false;
    try { run(); }
    catch (const Error &) { rejected = true; }
    require(rejected, message);
}

void put(std::vector<std::uint8_t> &bytes, std::size_t offset,
         std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i)
        bytes.at(offset + i) = static_cast<std::uint8_t>((value >> (i * 8)) & 255U);
}

Record block(const std::string &encoding, std::size_t length) {
    Record record;
    record.kind = 2;
    record.encoding = encoding;
    record.timestamp = 100;
    record.duration = 1000000;
    record.rate = 4;
    record.block.resize(length);
    return record;
}

std::vector<Point> points(const Record &record, const std::string &channel = "value") {
    std::vector<Point> result;
    decode(record, channel, [&](Point value) { result.push_back(value); });
    return result;
}

void codec_tests() {
    auto scalar = Record{};
    scalar.kind = 1;
    scalar.timestamp = 777;
    scalar.scalar = -12.5;
    const auto one = points(scalar);
    require(one.size() == 1 && one.front().timestamp == 777 &&
            one.front().value == -12.5 && !one.front().gap, "scalar decoding");
    scalar.quality = 3;
    require(points(scalar).front().value == -12.5,
            "decoder preserves flagged values for detail inspection");
    scalar.scalar.reset();
    rejects([&] { points(scalar); }, "missing scalar rejected");
    scalar.scalar = std::numeric_limits<double>::infinity();
    require(points(scalar).front().gap, "infinite scalar is gap");
    scalar.kind = 0;
    require(points(scalar).front().gap, "missing payload is gap");

    auto compact = block("s16le", 8);
    put(compact.block, 0, 0xffff, 2);
    put(compact.block, 2, 0x8000, 2);
    put(compact.block, 4, 0x7fff, 2);
    put(compact.block, 6, 0, 2);
    auto decoded = points(compact);
    require(decoded.size() == 4 && decoded[0].value == -1 &&
            decoded[1].value == -32768 && decoded[2].value == 32767,
            "s16le sign and extremes");
    require(decoded[3].timestamp == 750100, "sample-rate reconstructed timestamp");
    compact = block("s32le", 8);
    put(compact.block, 0, 0x80000000ULL, 4);
    put(compact.block, 4, 0x7fffffffULL, 4);
    decoded = points(compact);
    require(decoded[0].value == -2147483648.0 && decoded[1].value == 2147483647.0,
            "s32le extremes");

    auto floats = block("f32le", 12);
    put(floats.block, 0, 0x3fc00000, 4); // 1.5
    put(floats.block, 4, 0x7fc00000, 4); // NaN
    put(floats.block, 8, 0xff800000, 4); // -Inf
    decoded = points(floats);
    require(decoded[0].value == 1.5 && !decoded[0].gap &&
            decoded[1].gap && decoded[2].gap, "f32le finite values and gaps");
    auto doubles = block("f64le", 16);
    put(doubles.block, 0, 0xc004000000000000ULL, 8); // -2.5
    put(doubles.block, 8, 0x7ff0000000000000ULL, 8); // +Inf
    decoded = points(doubles);
    require(decoded[0].value == -2.5 && decoded[1].gap, "f64le portable decode");

    auto xyz = block("iio-le-24-v1", 48);
    put(xyz.block, 0, 0xfffe, 2);
    put(xyz.block, 2, 42, 2);
    put(xyz.block, 4, 0x8000, 2);
    put(xyz.block, 24, 123, 2);
    put(xyz.block, 26, 321, 2);
    require(points(xyz, "x")[0].value == -2 && points(xyz, "x")[1].value == 123 &&
            points(xyz, "y")[0].value == 42 && points(xyz, "z")[0].value == -32768,
            "XYZ IIO scan stride, offset and sign");
    auto sound = block("iio-le-16-v1", 32);
    put(sound.block, 0, 0x80000000ULL, 4);
    put(sound.block, 16, 66, 4);
    require(points(sound, "sound")[0].value == -2147483648.0 &&
            points(sound, "sound")[1].value == 66, "sound IIO stride and sign");

    auto feature = block("iio-le-72-v1", 72);
    feature.rate.reset();
    feature.duration = 1;
    put(feature.block, 0, 65535, 2);
    put(feature.block, 12, 0xff9c, 2);
    put(feature.block, 56, 0xa5, 1);
    require(points(feature, "x_high_freq_acc_rms")[0].value == 65535 &&
            points(feature, "temperature")[0].value == -100 &&
            points(feature, "startup_flags")[0].value == 165 &&
            points(feature, "temperature")[0].timestamp == feature.timestamp,
            "feature signed temperature, byte flags and record timestamp without rate");
    const auto feature_channels = channels("iio-le-72-v1");
    require(feature_channels.size() == 29, "all feature channels exposed");
    std::unordered_set<std::string> ids;
    for (const auto &channel : feature_channels)
        require(ids.emplace(channel.id).second && !channel.label.empty(), "stable unique channels");
    require(channels("unknown").empty() && channels("unknown", 99).empty() &&
            channels("", 1).front().id == "value", "opaque encoding and scalar channels");
    rejects([&] { points(xyz, "unknown"); }, "unknown channel rejected");
    auto opaque = block("opaque-v1", 4);
    rejects([&] { points(opaque); }, "unknown encoding rejected");
    for (const auto &encoding : {"s16le", "s32le", "f32le", "f64le",
                                  "iio-le-24-v1", "iio-le-16-v1", "iio-le-72-v1"}) {
        auto bad = block(encoding, 1);
        const auto channel = channels(encoding).front().id;
        rejects([&] { points(bad, channel); }, "truncated encoding rejected: " + std::string(encoding));
        bad.block.clear();
        rejects([&] { points(bad, channel); }, "empty encoding rejected: " + std::string(encoding));
    }
    compact = block("s16le", 4);
    compact.rate.reset();
    rejects([&] { points(compact); }, "missing rate rejected");
    compact.rate = 0;
    rejects([&] { points(compact); }, "zero rate rejected");
    compact.rate = std::numeric_limits<double>::quiet_NaN();
    rejects([&] { points(compact); }, "NaN rate rejected");
    compact.rate = 1;
    compact.duration = 1000000;
    rejects([&] { points(compact); }, "last sample at half-open end rejected");
    compact.duration = 1000001;
    require(points(compact).back().timestamp == 1000100, "last sample before half-open end accepted");
    compact.duration = 0;
    rejects([&] { points(compact); }, "zero-duration waveform rejected");
    compact.duration = 1000000;
    compact.timestamp = max_time - 1;
    rejects([&] { points(compact); }, "timestamp addition overflow/range rejected");
    compact.timestamp = 0;
    compact.rate = std::numeric_limits<double>::denorm_min();
    rejects([&] { points(compact); }, "huge sample period rejected before integral cast");
    compact = block("s16le", 4);
    compact.timestamp = max_time - 2;
    compact.duration = 2;
    compact.rate = 1000000;
    require(points(compact).back().timestamp == max_time - 1, "maximum supported timestamp");
    auto cancelled = std::make_shared<std::atomic_bool>(true);
    rejects([&] { decode(compact, "value", [](Point) {}, cancelled); }, "initial cancellation");
    cancelled->store(false);
    std::size_t calls = 0;
    rejects([&] {
        decode(compact, "value", [&](Point) { ++calls; cancelled->store(true); }, cancelled);
    }, "stream cancellation");
    require(calls == 1, "cancellation stops stream before another sample");
}

void metadata_tests() {
    require(unpack_metadata("").empty(), "empty metadata");
    const auto metadata = unpack_metadata("6:传感6:测试1:x4:a:b;");
    require(metadata.size() == 2 && metadata[0].first == "传感" &&
            metadata[0].second == "测试" && metadata[1].second == "a:b;",
            "metadata uses UTF-8 byte lengths and permits delimiters");
    require(unpack_metadata("1:k0:").front().second.empty(), "empty metadata value");
    for (const auto &bad : {"x:a", "1:a", "4:abc", "1:a1:b1:a1:c", "0:1:v",
                            "999999999:x", "1:a-1:b", "1:a1:\xff", "1:a2:\xc0\x80"})
        rejects([&] { unpack_metadata(bad); }, "malformed metadata rejected");
    rejects([&] { unpack_metadata(std::string(65537, 'x')); }, "oversized metadata rejected");
    require(quality_text(0) == "good" && quality_text(15).find("partial") != std::string::npos &&
            quality_text(16).find("unknown") != std::string::npos, "quality bit descriptions");
}

void playback_tests() {
    using Microseconds = std::chrono::microseconds;
    PlaybackClock clock({100, 200});
    require(clock.position() == 100 && clock.state() == PlaybackState::stopped,
            "clock initially stopped at start");
    clock.advance(Microseconds{50});
    require(clock.position() == 100, "stopped time is frozen");
    clock.play();
    clock.advance(Microseconds{20});
    clock.pause();
    clock.advance(Microseconds{30});
    require(clock.position() == 120 && clock.state() == PlaybackState::paused,
            "paused time is frozen");
    clock.seek(130);
    require(clock.state() == PlaybackState::paused, "paused seek stays paused");
    clock.play();
    clock.seek(140);
    require(clock.state() == PlaybackState::playing, "playing seek stays playing");
    clock.advance(Microseconds{60});
    require(clock.position() == 200 && clock.state() == PlaybackState::finished,
            "clock reaches end without overshoot");
    clock.play();
    require(clock.position() == 100 && clock.state() == PlaybackState::playing,
            "play after finish replays from start");
    clock.seek(200);
    require(clock.state() == PlaybackState::finished, "explicit seek to end finishes");
    clock.seek(150);
    require(clock.state() == PlaybackState::paused, "seek back from finished pauses");
    clock.stop();
    require(clock.position() == 100 && clock.state() == PlaybackState::stopped,
            "stop resets to start");
    rejects([&] { clock.seek(99); }, "seek before start rejected");
    rejects([&] { clock.seek(201); }, "seek after end rejected");
    rejects([&] { clock.advance(Microseconds{-1}); }, "negative elapsed rejected");
    for (const auto speed : {0.0, -1.0, 1001.0, 0.009,
                            std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN()})
        rejects([&] { clock.set_speed(speed); }, "invalid speed rejected");
    clock.set_speed(0.1);
    clock.play();
    for (int i = 0; i < 10; ++i)
        clock.advance(Microseconds{1});
    require(clock.position() == 101, "fractional speed retains sub-microsecond time");
    clock.stop();
    clock.set_speed(0.01);
    clock.play();
    for (int i = 0; i < 100; ++i)
        clock.advance(Microseconds{1});
    require(clock.position() == 101, "minimum speed accumulates fractions");
    clock.set_range({max_time - 10, max_time});
    clock.set_speed(1000);
    clock.play();
    clock.advance(Microseconds{std::numeric_limits<std::int64_t>::max()});
    require(clock.position() == max_time && clock.state() == PlaybackState::finished,
            "huge elapsed/speed saturates without overflow");
    for (const Range invalid : {Range{-1, 10}, Range{0, 0}, Range{10, 9},
                               Range{0, max_time + 1}})
        rejects([&] { clock.set_range(invalid); }, "invalid range rejected");
    require(clock.range().start == max_time - 10 && clock.range().end == max_time,
            "invalid range does not change state");
}

void audio_and_spectrum_tests() {
    AudioClip clip;
    clip.sample_rate_hz = 16000;
    clip.range = {1000, 1001000};
    clip.pcm16 = {0, 16384, -16384, 32767, -32768};
    const auto wav = encode_wav(clip);
    require(wav.size() == 44 + clip.pcm16.size() * 2, "WAV size matches header + 16-bit PCM");
    require(std::memcmp(wav.data(), "RIFF", 4) == 0 &&
            std::memcmp(wav.data() + 8, "WAVEfmt ", 8) == 0 &&
            std::memcmp(wav.data() + 36, "data", 4) == 0,
            "WAV canonical RIFF/WAVE/fmt/data markers");
    rejects([&] {
        AudioClip empty_clip;
        empty_clip.sample_rate_hz = 16000;
        (void)encode_wav(empty_clip);
    }, "empty WAV rejected");
    rejects([&] {
        AudioClip bad_rate = clip;
        bad_rate.sample_rate_hz = 0;
        (void)encode_wav(bad_rate);
    }, "zero sample-rate WAV rejected");

    std::vector<double> tone(1024);
    for (std::size_t i = 0; i < tone.size(); ++i)
        tone[i] = 50.0 + 100.0 * std::sin(2.0 * 3.14159265358979323846 * 80.0 *
                                          static_cast<double>(i) / 1000.0);
    const auto spec = compute_spectrum(tone, 1000.0, 512);
    require(spec.samples_used == 1024 && !spec.bins.empty(), "spectrum bin count");
    require(std::abs(spec.dominant_frequency_hz - 80.0) < 1.5 && spec.dominant_magnitude > 80.0,
            "spectrum identifies 80 Hz tone despite DC offset");
    rejects([&] { (void)compute_spectrum({1.0, 2.0}, 1000.0); }, "too few samples rejected");
    rejects([&] { (void)compute_spectrum(tone, 0.0); }, "zero spectrum sample rate rejected");

    AudioPlayer player;
    require(!player.has_clip() && !player.is_playing(), "player initially empty");
    player.load(clip);
    require(player.has_clip() && player.sample_rate_hz() == 16000, "player loaded clip");
    player.set_volume(0.5);
    require(std::abs(player.volume() - 0.5) < 1e-6, "player volume");
    rejects([&] { player.set_volume(1.5); }, "invalid volume rejected");
    player.set_muted(true);
    require(player.muted(), "player muted");
    player.set_muted(false);
    player.seek(500000);
    require(player.position() >= clip.range.start && player.position() <= clip.range.end,
            "player seek position");
    player.stop();
    player.clear();
    require(!player.has_clip(), "player cleared");
}
} // namespace

int main() {
    try {
        codec_tests();
        metadata_tests();
        playback_tests();
        audio_and_spectrum_tests();
        std::cout << "codec, metadata, playback, audio and spectrum tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "codec/playback test failure: " << error.what() << '\n';
        return 1;
    }
}
