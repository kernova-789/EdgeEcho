#pragma once

#include "edgeecho/core.hpp"
#include <edgesense_replay/types.hpp>

namespace edgeecho::detail {
namespace replay = edgesense::replay;

void require(bool condition, const std::string &message);
void validate_range(Range range);
[[noreturn]] void throw_replay_error(const replay::Error &error);

class ReadBudget {
  public:
    explicit ReadBudget(Cancel cancel);
    void reset(std::chrono::milliseconds timeout);
    bool expired() const noexcept;
    void check() const;
    std::chrono::milliseconds remaining() const;
    const Cancel &cancellation() const noexcept { return cancel_; }

  private:
    Cancel cancel_;
    std::chrono::steady_clock::time_point deadline_;
};
} // namespace edgeecho::detail
