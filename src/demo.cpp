#include "atomic_output.hpp"
#include "archive_support.hpp"
#include <edgesense_replay/store.hpp>

#include <array>
#include <cmath>
#include <fstream>

namespace edgeecho {
namespace {
namespace replay = detail::replay;
constexpr Time anchor = 1793145600000000LL; // 2026-10-28 00:00:00 UTC
constexpr double tau = 6.283185307179586;

class DemoWorkspace {
  public:
    explicit DemoWorkspace(std::filesystem::path path) : path_(std::move(path)) {
        path_ += ".replay";
        std::error_code error;
        if (!std::filesystem::create_directory(path_, error))
            throw Error("Cannot create demo workspace: " + (error ? error.message() : "already exists"));
    }

    ~DemoWorkspace() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

void put(replay::Bytes &block, std::size_t offset, std::uint64_t value, unsigned width) {
    for (unsigned byte = 0; byte < width; ++byte)
        block.at(offset + byte) = static_cast<std::uint8_t>((value >> (byte * 8)) & 255U);
}

replay::Record demo_record(std::string id, const char *sensor, const char *type, Time timestamp) {
    replay::Record record;
    record.id = std::move(id);
    record.sensor_id = sensor;
    record.data_type = type;
    record.unit = "count";
    record.timestamp = timestamp;
    record.source = "EdgeEcho demo (synthetic)";
    record.batch_id = "demo";
    record.metadata.emplace("input_origin", "simulated");
    return record;
}

void write_scalars(replay::Store &store, std::size_t count) {
    constexpr std::size_t batch_size = 1024;
    std::vector<replay::Record> batch;
    batch.reserve(batch_size);
    for (std::size_t n = 0; n < count; ++n) {
        auto record = demo_record("scalar-" + std::to_string(n), "demo-vibration", "acceleration_rms",
                                  anchor + static_cast<Time>(n) * 10000);
        record.value = 10 + std::sin(static_cast<double>(n) * 0.035) * 4 + (n % 997 == 0 ? 30 : 0);
        if (n % 503 == 502) record.quality = replay::uncertain_time;
        batch.push_back(std::move(record));
        if (batch.size() == batch_size || n + 1 == count) {
            store.append(batch);
            batch.clear();
        }
    }
}

enum class Stream { vibration, sound, feature };

void write_stream(replay::Store &store, Stream stream, const char *sensor, const char *type,
                  const char *encoding, std::size_t stride) {
    const bool feature = stream == Stream::feature;
    const auto samples = feature ? 1U : 1000U;
    std::vector<replay::Record> blocks;
    for (int batch = 0; batch < 20; ++batch) {
        const auto timestamp = anchor + batch * replay::second;
        auto record = demo_record("block-" + std::to_string(static_cast<int>(stream)) + "-" +
                                  std::to_string(batch), sensor, type, timestamp);
        record.encoding = encoding;
        record.duration_us = feature ? 1 : replay::second;
        if (!feature) record.sample_rate_hz = 1000;
        replay::Bytes block(samples * stride, 0);
        for (std::size_t sample = 0; sample < samples; ++sample) {
            const auto time = static_cast<double>(batch) + static_cast<double>(sample) / 1000;
            const auto offset = sample * stride;
            const auto nanoseconds = static_cast<std::uint64_t>(timestamp +
                static_cast<Time>(sample) * 1000) * 1000;
            switch (stream) {
                case Stream::vibration:
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        const auto value = static_cast<std::int16_t>(
                            10000 * std::sin(time * tau * static_cast<double>(5 + axis)));
                        put(block, offset + axis * 2, static_cast<std::uint16_t>(value), 2);
                    }
                    put(block, offset + 8, sample / 64, 4);
                    put(block, offset + 12, sample % 64, 2);
                    put(block, offset + 16, nanoseconds, 8);
                    break;
                case Stream::sound: {
                    const auto value = static_cast<std::int32_t>(1000000 * std::sin(time * tau * 80));
                    put(block, offset, static_cast<std::uint32_t>(value), 4);
                    put(block, offset + 4, sample / 64, 2);
                    put(block, offset + 6, sample % 64, 2);
                    put(block, offset + 8, nanoseconds, 8);
                    break;
                }
                case Stream::feature:
                    for (unsigned channel = 0; channel < 28; ++channel)
                        put(block, channel * 2, 100 + channel * 20 + static_cast<unsigned>(batch), 2);
                    put(block, 12, static_cast<unsigned>(2500 + batch), 2);
                    put(block, 56, 3, 1);
                    put(block, 64, nanoseconds, 8);
                    break;
            }
        }
        record.value = std::move(block);
        blocks.push_back(std::move(record));
    }
    store.append(blocks);
}

void write_demo(const std::filesystem::path &directory, std::size_t count) {
    replay::Config config;
    config.directory = directory;
    config.max_database_bytes = 1ULL << 40;
    replay::Store store(config);
    write_scalars(store, count);
    write_stream(store, Stream::vibration, "demo-vibration", "vibration_iio_block", "iio-le-24-v1", 24);
    write_stream(store, Stream::sound, "demo-sound", "raw_sound_iio", "iio-le-16-v1", 16);
    write_stream(store, Stream::feature, "demo-vibration", "feature_iio_scan", "iio-le-72-v1", 72);

    replay::Event event;
    event.id = "demo-event";
    event.sensor_id = "demo-vibration";
    event.start = anchor + 3 * replay::second;
    event.end = anchor + 7 * replay::second;
    event.assessment = replay::Assessment::warning;
    event.severity = replay::Severity::warning;
    event.event_type = "demo_warning";
    event.description = "Synthetic event for replay demonstration";
    event.source = "EdgeEcho";
    event.model_version = "demo";
    store.create_event(event, {"demo-create", event.start, "demo", "synthetic"});
}
} // namespace

void create_demo(const std::filesystem::path &target, std::size_t count) {
    detail::require(count > 0 && count <= 10000000, "Demo record count must be in 1..10000000");
    detail::AtomicOutput output(std::filesystem::weakly_canonical(std::filesystem::absolute(target)));
    const DemoWorkspace workspace(output.temporary_path());
    const auto database = workspace.path() / "replay.db";
    try {
        write_demo(workspace.path(), count);
        // All writer connections have closed. Only publish a standalone, validated database.
        replay::Store::open_offline(database);
    } catch (const replay::Error &error) {
        detail::throw_replay_error(error);
    }

    std::ifstream input(database, std::ios::binary);
    detail::require(static_cast<bool>(input), "Cannot read generated demo database");
    std::array<char, 64 * 1024> buffer{};
    do {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        output.write(std::string_view(buffer.data(), static_cast<std::size_t>(input.gcount())));
    } while (input);
    detail::require(input.eof() && !input.bad(), "Cannot read generated demo database");
    output.publish();
}
} // namespace edgeecho
