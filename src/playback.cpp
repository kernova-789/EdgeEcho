#include "edgeecho/core.hpp"

#include <cmath>

namespace edgeecho {
PlaybackClock::PlaybackClock(Range range) {
    set_range(range);
}

void PlaybackClock::set_range(Range range) {
    if (range.start < 0 || range.end <= range.start || range.end > max_time)
        throw Error("playback requires a nonempty UTC range [start,end)");
    range_ = range;
    position_ = range.start;
    fraction_ = 0;
    state_ = PlaybackState::stopped;
}

void PlaybackClock::play() {
    if (state_ == PlaybackState::finished) {
        position_ = range_.start;
        fraction_ = 0;
    }
    state_ = PlaybackState::playing;
}

void PlaybackClock::pause() {
    if (state_ == PlaybackState::playing)
        state_ = PlaybackState::paused;
}

void PlaybackClock::stop() {
    position_ = range_.start;
    fraction_ = 0;
    state_ = PlaybackState::stopped;
}

void PlaybackClock::seek(Time target) {
    if (target < range_.start || target > range_.end)
        throw Error("seek target is outside the playback range");
    position_ = target;
    fraction_ = 0;
    if (target == range_.end)
        state_ = PlaybackState::finished;
    else if (state_ != PlaybackState::playing)
        state_ = PlaybackState::paused;
}

void PlaybackClock::set_speed(double speed) {
    if (!std::isfinite(speed) || speed < 0.01 || speed > 1000)
        throw Error("playback speed must be finite and in [0.01,1000]");
    speed_ = speed;
}

Time PlaybackClock::advance(std::chrono::microseconds elapsed) {
    if (elapsed.count() < 0)
        throw Error("playback elapsed time cannot be negative");
    if (state_ != PlaybackState::playing)
        return position_;
    const auto delta = static_cast<long double>(elapsed.count()) *
                       static_cast<long double>(speed_) + fraction_;
    const auto remaining = range_.end - position_;
    if (delta >= static_cast<long double>(remaining)) {
        position_ = range_.end;
        fraction_ = 0;
        state_ = PlaybackState::finished;
        return position_;
    }
    const auto whole = static_cast<Time>(delta);
    fraction_ = delta - static_cast<long double>(whole);
    position_ += whole;
    return position_;
}
} // namespace edgeecho
