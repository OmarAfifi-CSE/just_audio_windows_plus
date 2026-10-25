#pragma once

#include <cmath>
#include <cstdint>

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
