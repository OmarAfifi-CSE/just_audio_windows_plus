#pragma once

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

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

// Normalizes a WinRT TimeSpan tick count (100 ns units) to positive
// microseconds, returning 0 for non-positive or unbounded/sentinel durations
// (such as WinRT TimeSpan::max() reported on live streams).
inline int64_t NormalizeDurationMicroseconds(int64_t raw_ticks) {
  constexpr int64_t kMaxReasonableTicks =
      (std::numeric_limits<int64_t>::max)() / 2;
  if (raw_ticks <= 0 || raw_ticks >= kMaxReasonableTicks) {
    return 0;
  }
  return raw_ticks / 10;
}

inline std::string TrimDiagnosticText(const std::string& text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

inline std::string FormatHResultHex(int32_t hr) {
  std::ostringstream out;
  out << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
      << static_cast<uint32_t>(hr);
  return out.str();
}

// Builds a deterministic, locale-independent playback diagnostic message that
// preserves WinRT error categories and HRESULT codes alongside any OS text.
inline std::string FormatWinRtPlaybackError(const std::string& category,
                                            int32_t hresult,
                                            const std::string& os_message) {
  const auto trimmed_category = TrimDiagnosticText(category);
  const auto trimmed_message = TrimDiagnosticText(os_message);
  if (trimmed_category.empty() && hresult == 0 && trimmed_message.empty()) {
    return "Native media source failed";
  }

  std::string prefix;
  if (!trimmed_category.empty()) {
    if (hresult != 0 && trimmed_category.back() == ')') {
      prefix = trimmed_category.substr(0, trimmed_category.size() - 1) +
               ", " + FormatHResultHex(hresult) + ")";
    } else if (hresult != 0) {
      prefix = trimmed_category + " (" + FormatHResultHex(hresult) + ")";
    } else {
      prefix = trimmed_category;
    }
  } else if (hresult != 0) {
    prefix = "WinRT error (" + FormatHResultHex(hresult) + ")";
  }

  if (trimmed_message.empty()) return prefix;
  if (prefix.empty()) return trimmed_message;
  return prefix + ": " + trimmed_message;
}
