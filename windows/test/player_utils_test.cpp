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

}  // namespace test
}  // namespace just_audio_windows_plus
