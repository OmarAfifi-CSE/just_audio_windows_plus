#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Applies a permutation expressed as source indices to a sequence. Invalid
// permutations are rejected instead of silently producing a corrupt playlist.
template <typename T>
bool ReorderByShuffleOrder(const std::vector<T>& source,
                           const std::vector<int64_t>& order,
                           std::vector<T>& result) {
  if (order.size() != source.size()) {
    return false;
  }

  std::vector<bool> seen(source.size(), false);
  result.clear();
  result.reserve(source.size());

  for (const auto index : order) {
    if (index < 0 || static_cast<size_t>(index) >= source.size() ||
        seen[static_cast<size_t>(index)]) {
      result.clear();
      return false;
    }
    seen[static_cast<size_t>(index)] = true;
    result.push_back(source[static_cast<size_t>(index)]);
  }
  return true;
}

inline int64_t ClampBufferedPosition(int64_t duration, double progress) {
  if (duration <= 0) {
    return 0;
  }
  if (!std::isfinite(progress) || progress <= 0.0) {
    return 0;
  }
  if (progress >= 1.0) {
    return duration;
  }
  return static_cast<int64_t>(duration * progress);
}
