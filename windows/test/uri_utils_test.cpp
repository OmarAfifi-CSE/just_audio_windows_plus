#include "../uri_utils.hpp"
#include "../native_utils.hpp"

#include <gtest/gtest.h>
#include <limits>

namespace just_audio_windows_plus {
namespace test {

// ── EncodeSpacesInUri ────────────────────────────────────────────────────────

// A plain URL with no spaces should pass through unchanged.
TEST(EncodeSpacesInUri, NoSpaces_ReturnsUnchanged) {
  EXPECT_EQ(EncodeSpacesInUri("https://example.com/audio.mp3"),
            "https://example.com/audio.mp3");
}

// An empty string should stay empty.
TEST(EncodeSpacesInUri, EmptyString_ReturnsEmpty) {
  EXPECT_EQ(EncodeSpacesInUri(""), "");
}

// Literal spaces in a local file path must be encoded as %20.
// This is the core regression for https://github.com/bdlukaa/just_audio_windows/issues/26.
TEST(EncodeSpacesInUri, LiteralSpacesInFilePath_EncodedAsPercent20) {
  EXPECT_EQ(
      EncodeSpacesInUri("file:///C:/Users/My Files/audio.mp3"),
      "file:///C:/Users/My%20Files/audio.mp3");
}

// Multiple consecutive spaces must each be individually encoded.
TEST(EncodeSpacesInUri, MultipleSpaces_AllEncoded) {
  EXPECT_EQ(
      EncodeSpacesInUri(
          "C:/Users/HP/Downloads/Recording Session File.mp3"),
      "C:/Users/HP/Downloads/Recording%20Session%20File.mp3");
}

// Already percent-encoded spaces (%20) must NOT be double-encoded.
// This ensures a properly-encoded file:// URI like those produced by
// Dart's Uri.file() is left intact.
TEST(EncodeSpacesInUri, AlreadyEncodedSpaces_NotDoubleEncoded) {
  EXPECT_EQ(
      EncodeSpacesInUri("file:///C:/Users/My%20Files/audio.mp3"),
      "file:///C:/Users/My%20Files/audio.mp3");
}

// Percent-encoded multi-byte UTF-8 sequences (e.g. U+2019 RIGHT SINGLE
// QUOTATION MARK) must pass through unchanged so that Windows::Foundation::Uri
// can decode them natively.
// This is the core regression for the original apostrophe bug.
TEST(EncodeSpacesInUri, MultiBytePercentEncoded_Unchanged) {
  EXPECT_EQ(
      EncodeSpacesInUri(
          "https://example.com/speech?text=I%E2%80%99d%20like%20a%20coffee"),
      "https://example.com/speech?text=I%E2%80%99d%20like%20a%20coffee");
}

// A URL with both unencoded spaces and percent-encoded multi-byte chars:
// spaces must be encoded while the percent-sequences stay intact.
TEST(EncodeSpacesInUri, MixedLiteralSpacesAndPercentEncoded) {
  EXPECT_EQ(
      EncodeSpacesInUri(
          "https://example.com/speech?text=I%E2%80%99d like a coffee"),
      "https://example.com/speech?text=I%E2%80%99d%20like%20a%20coffee");
}

// A URL with only query-string spaces should have them encoded.
TEST(EncodeSpacesInUri, SpaceInQueryString) {
  EXPECT_EQ(
      EncodeSpacesInUri("https://example.com/audio?title=recording session"),
      "https://example.com/audio?title=recording%20session");
}

// Arabic Unicode file paths with literal spaces must encode spaces and preserve UTF-8 bytes.
TEST(EncodeSpacesInUri, ArabicUnicodePathWithSpaces) {
  EXPECT_EQ(
      EncodeSpacesInUri("file:///C:/تلاوات قرآنية/سورة الفاتحة.mp3"),
      "file:///C:/تلاوات%20قرآنية/سورة%20الفاتحة.mp3");
}

// Windows UNC network paths with spaces must be encoded cleanly.
TEST(EncodeSpacesInUri, UncNetworkPathWithSpaces) {
  EXPECT_EQ(
      EncodeSpacesInUri(R"(\\server\shared drive\audio track.wav)"),
      R"(\\server\shared%20drive\audio%20track.wav)");
}

// Leading and trailing spaces must be handled safely.
TEST(EncodeSpacesInUri, LeadingAndTrailingSpaces) {
  EXPECT_EQ(
      EncodeSpacesInUri(" audio.mp3 "),
      "%20audio.mp3%20");
}

// URL fragments with spaces should be encoded.
TEST(EncodeSpacesInUri, FragmentWithSpaces) {
  EXPECT_EQ(
      EncodeSpacesInUri("https://example.com/audio.mp3#timestamp 01:23"),
      "https://example.com/audio.mp3#timestamp%2001:23");
}

// Large strings with repeated spaces must not overflow and maintain capacity.
TEST(EncodeSpacesInUri, LongStringStressTest) {
  std::string longInput(500, ' ');
  std::string expected;
  for (int i = 0; i < 500; ++i) expected += "%20";
  EXPECT_EQ(EncodeSpacesInUri(longInput), expected);
}

// Arabic paths with full Tashkeel and punctuation.
TEST(EncodeSpacesInUri, ArabicWithFullTashkeelAndPunctuation) {
  EXPECT_EQ(
      EncodeSpacesInUri(
          "file:///C:/تلاوات خاشعة/الشيخ محمد صِدِّيق المِنْشَاوِيّ (رَحِمَهُ اللَّهُ)/001 - سُورَةُ الفَاتِحَةِ [مَكِّيَّة].mp3"),
      "file:///C:/تلاوات%20خاشعة/الشيخ%20محمد%20صِدِّيق%20المِنْشَاوِيّ%20(رَحِمَهُ%20اللَّهُ)/001%20-%20سُورَةُ%20الفَاتِحَةِ%20[مَكِّيَّة].mp3");
}

// URLs with ports, IP addresses, and spaces in path.
TEST(EncodeSpacesInUri, LocalNetworkIpAndPortWithSpaces) {
  EXPECT_EQ(
      EncodeSpacesInUri("http://192.168.1.100:8080/quran streams/001 minshawi.mp3"),
      "http://192.168.1.100:8080/quran%20streams/001%20minshawi.mp3");
}

// Consecutive 10 spaces in a row.
TEST(EncodeSpacesInUri, TenConsecutiveSpaces) {
  EXPECT_EQ(
      EncodeSpacesInUri("quran          recitation.mp3"),
      "quran%20%20%20%20%20%20%20%20%20%20recitation.mp3");
}

// Paths with mixed special characters (brackets, plus, ampersand, hash).
TEST(EncodeSpacesInUri, SpecialCharactersInPathWithSpaces) {
  EXPECT_EQ(
      EncodeSpacesInUri("file:///C:/Audio [HQ] (2026) + Minshawi & Holy Quran/track 01.mp3"),
      "file:///C:/Audio%20[HQ]%20(2026)%20+%20Minshawi%20&%20Holy%20Quran/track%2001.mp3");
}

// Extreme stress test: 10,000 characters alternating char and space.
TEST(EncodeSpacesInUri, TenThousandCharacterAlternatingStressTest) {
  std::string input;
  std::string expected;
  input.reserve(10000);
  expected.reserve(20000);

  for (int i = 0; i < 5000; ++i) {
    input += "a ";
    expected += "a%20";
  }

  EXPECT_EQ(EncodeSpacesInUri(input), expected);
}

TEST(ReorderByShuffleOrder, AppliesPermutationFromOriginalIndices) {
  const std::vector<int> source{0, 1, 2};
  std::vector<int> result;

  ASSERT_TRUE(ReorderByShuffleOrder(source, std::vector<int64_t>{1, 2, 0}, result));
  EXPECT_EQ(result, (std::vector<int>{1, 2, 0}));
}

TEST(ReorderByShuffleOrder, RejectsDuplicateOrOutOfRangeIndices) {
  const std::vector<int> source{0, 1, 2};
  std::vector<int> result{9};

  EXPECT_FALSE(ReorderByShuffleOrder(source, std::vector<int64_t>{0, 0, 2}, result));
  EXPECT_TRUE(result.empty());
  EXPECT_FALSE(ReorderByShuffleOrder(source, std::vector<int64_t>{0, 1, 3}, result));
}

TEST(ClampBufferedPosition, KeepsPositionWithinDuration) {
  EXPECT_EQ(ClampBufferedPosition(100, -1.0), 0);
  EXPECT_EQ(ClampBufferedPosition(100, 0.5), 50);
  EXPECT_EQ(ClampBufferedPosition(100, 2.0), 100);
  EXPECT_EQ(ClampBufferedPosition(100, std::numeric_limits<double>::quiet_NaN()), 0);
  EXPECT_EQ(ClampBufferedPosition(0, 0.5), 0);
}

}  // namespace test
}  // namespace just_audio_windows_plus
