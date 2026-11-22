#include "../player.hpp"

#include <gtest/gtest.h>
#include <limits>

namespace just_audio_windows_plus {
namespace test {

// TimeSpan counts 100 ns ticks, so a microsecond value near int64_t max
// would overflow the tick conversion. Every caller-supplied position must go
// through MicrosToTimeSpan so extreme inputs saturate instead of invoking UB.
TEST(PlayerTimeUtils, MicrosToTimeSpanRoundTripsOrdinaryPositions) {
  EXPECT_EQ(TimeSpanToMicroseconds(MicrosToTimeSpan(0)), 0);
  EXPECT_EQ(TimeSpanToMicroseconds(MicrosToTimeSpan(1)), 1);
  EXPECT_EQ(TimeSpanToMicroseconds(MicrosToTimeSpan(1500000)), 1500000);
  EXPECT_EQ(TimeSpanToMicroseconds(MicrosToTimeSpan(3600000000LL)), 3600000000LL);
}

TEST(PlayerTimeUtils, MicrosToTimeSpanClampsToRepresentableRange) {
  constexpr int64_t kMaxUs = (std::numeric_limits<int64_t>::max)() / 10;
  EXPECT_EQ(TimeSpanToMicroseconds(MicrosToTimeSpan(kMaxUs)), kMaxUs);
  EXPECT_EQ(TimeSpanToMicroseconds(MicrosToTimeSpan(kMaxUs + 1)), kMaxUs);
  EXPECT_EQ(TimeSpanToMicroseconds(MicrosToTimeSpan((std::numeric_limits<int64_t>::max)())),
            kMaxUs);
  // Negative inputs are already rejected by validation; the helper must still
  // never construct a negative (UB-prone) duration.
  EXPECT_EQ(TimeSpanToMicroseconds(MicrosToTimeSpan(-1)), 0);
  EXPECT_EQ(TimeSpanToMicroseconds(MicrosToTimeSpan((std::numeric_limits<int64_t>::min)())), 0);
}

TEST(PlayerTimeUtils, NormalizeDurationMicrosecondsRejectsUnboundedAndNonPositive) {
  EXPECT_EQ(NormalizeDurationMicroseconds(0), 0);
  EXPECT_EQ(NormalizeDurationMicroseconds(-50), 0);
  EXPECT_EQ(NormalizeDurationMicroseconds(30000000LL), 3000000LL);
  EXPECT_EQ(
      NormalizeDurationMicroseconds((std::numeric_limits<int64_t>::max)()), 0);
  EXPECT_EQ(
      NormalizeDurationMicroseconds((std::numeric_limits<int64_t>::max)() / 2),
      0);
}

TEST(PlayerDiagnosticUtils, FormatsCategoryHResultAndTrimmedMessage) {
  EXPECT_EQ(
      FormatWinRtPlaybackError(
          "Unsupported format (SourceNotSupported)",
          static_cast<int32_t>(0xC00D36C4),
          "  The byte stream type is unsupported.\r\n"),
      "Unsupported format (SourceNotSupported, 0xC00D36C4): The byte stream type is unsupported.");
  EXPECT_EQ(FormatWinRtPlaybackError("Decoder failed (DecodingError)", 0, ""),
            "Decoder failed (DecodingError)");
  EXPECT_EQ(
      FormatWinRtPlaybackError(
          "",
          static_cast<int32_t>(0x80070002),
          "The system cannot find the path specified."),
      "WinRT error (0x80070002): The system cannot find the path specified.");
  EXPECT_EQ(FormatWinRtPlaybackError("", 0, "  \r\n "),
            "Native media source failed");
}

namespace {
class RecordingBinaryMessenger final : public flutter::BinaryMessenger {
 public:
  int set_handler_null_calls = 0;
  std::string last_channel;
  mutable int send_calls = 0;

  void Send(const std::string&,
            const uint8_t*,
            size_t,
            flutter::BinaryReply = nullptr) const override {
    ++send_calls;
  }

  void SetMessageHandler(const std::string& channel,
                         flutter::BinaryMessageHandler handler) override {
    if (!handler) {
      ++set_handler_null_calls;
      last_channel = channel;
    }
  }
};
}  // namespace

TEST(PlayerTeardownGuard, SkipsMessengerUnregisterWhenUnavailable) {
  RecordingBinaryMessenger messenger;
  bool available = true;
  EXPECT_TRUE(IsMessengerAvailable([&] { return available; }));

  available = false;
  EXPECT_FALSE(IsMessengerAvailable([&] { return available; }));
  UnregisterHandlerIfAvailable(
      &messenger, "test.events.shutdown", [&] { return available; });

  EXPECT_EQ(messenger.set_handler_null_calls, 0);
  EXPECT_EQ(messenger.send_calls, 0);
}

TEST(PlayerTeardownGuard, UnregistersMessengerWhenAvailable) {
  RecordingBinaryMessenger messenger;
  UnregisterHandlerIfAvailable(
      &messenger, "test.events.normal", [] { return true; });
  EXPECT_EQ(messenger.set_handler_null_calls, 1);
  EXPECT_EQ(messenger.last_channel, "test.events.normal");

  // Default/empty predicate treats messenger as available; null messenger is safe.
  EXPECT_TRUE(IsMessengerAvailable({}));
  UnregisterHandlerIfAvailable(nullptr, "test.events.null", {});
  EXPECT_EQ(messenger.set_handler_null_calls, 1);
}

}  // namespace test
}  // namespace just_audio_windows_plus
