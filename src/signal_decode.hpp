#pragma once

#include "edgeecho/core.hpp"
#include <edgesense_replay/signal.hpp>

namespace edgeecho::detail {
void decode_window(const edgesense::replay::SignalView &, const std::string &channel, Range,
                   const std::function<void(Point)> &, const Cancel &);
} // namespace edgeecho::detail
