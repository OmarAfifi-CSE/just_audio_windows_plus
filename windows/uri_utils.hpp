#pragma once

#include <cctype>
#include <cstdint>
#include <string>

inline bool StartsWithIgnoreCase(const std::string& text,
                                 const std::string& prefix) {
  if (text.length() < prefix.length()) return false;
  for (size_t i = 0; i < prefix.length(); ++i) {
    if (std::tolower(static_cast<unsigned char>(text[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

inline bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
  if (a.length() != b.length()) return false;
  for (size_t i = 0; i < a.length(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

// Encodes literal space characters in a URI string as "%20" so that
// Windows::Foundation::Uri accepts URIs that contain unencoded spaces
// (e.g. local file paths like "file:///C:/My Files/audio.mp3").
//
// Already percent-encoded sequences such as "%20" or "%E2%80%99" are never
// modified, because they contain no literal space character.
inline std::string EncodeSpacesInUri(const std::string& uri) {
  std::string encoded;
  // Reserve worst-case capacity (every char is a space → 3 chars each).
  encoded.reserve(uri.length() * 3);
  for (char c : uri) {
    if (c == ' ') {
      encoded += "%20";
    } else {
      encoded += c;
    }
  }
  return encoded;
}

// Decodes every "%XX" escape in |text| back into its byte, so a URI path can be
// turned into the text it represents. Invalid escapes and "%00" (embedded NUL)
// are kept literally, so malformed input is never silently rewritten or
// truncated at C-string boundaries.
inline std::string DecodePercentEscapes(const std::string& text) {
  std::string decoded;
  decoded.reserve(text.length());
  for (size_t i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if (c != '%' || i + 2 >= text.length()) {
      decoded += c;
      continue;
    }
    const auto hex_value = [](char digit) -> int {
      if (digit >= '0' && digit <= '9') return digit - '0';
      if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
      if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
      return -1;
    };
    const int high = hex_value(text[i + 1]);
    const int low = hex_value(text[i + 2]);
    if (high < 0 || low < 0) {
      decoded += c;
      continue;
    }
    const int byte = (high << 4) | low;
    if (byte == 0) {
      decoded += c;
      continue;
    }
    decoded += static_cast<char>(byte);
    i += 2;
  }
  return decoded;
}

// Escapes the characters Windows::Foundation::Uri rejects when they appear
// literally: space, '%', '#' and '?'. Non-ASCII characters stay literal so Uri
// encodes them itself, which is the point of decoding first - WinRT's media
// pipeline cannot resolve a percent-encoded UTF-8 file path.
inline std::string EscapeUriPath(const std::string& decoded) {
  std::string escaped;
  escaped.reserve(decoded.length());
  for (char c : decoded) {
    switch (c) {
      case '%':
        escaped += "%25";
        break;
      case ' ':
        escaped += "%20";
        break;
      case '#':
        escaped += "%23";
        break;
      case '?':
        escaped += "%3F";
        break;
      default:
        escaped += c;
        break;
    }
  }
  return escaped;
}

inline bool TryParsePercentByte(const std::string& text,
                                size_t pos,
                                uint8_t& out) {
  if (pos + 2 >= text.length() || text[pos] != '%') return false;
  const auto hex_value = [](char digit) -> int {
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    return -1;
  };
  const int high = hex_value(text[pos + 1]);
  const int low = hex_value(text[pos + 2]);
  if (high < 0 || low < 0) return false;
  out = static_cast<uint8_t>((high << 4) | low);
  return true;
}

// Decodes valid multi-byte UTF-8 percent-escape sequences while preserving all
// ASCII escapes (%00, %20, %23, %25, %2F, %3F, %7B, etc.) and malformed,
// overlong, or surrogate sequences literally. WinRT's Uri rejects raw RFC-unsafe
// ASCII chars and cannot resolve percent-encoded UTF-8 local paths.
inline std::string DecodeUtf8PercentEscapes(const std::string& text) {
  std::string decoded;
  decoded.reserve(text.length());
  size_t i = 0;
  while (i < text.length()) {
    uint8_t b0 = 0;
    if (!TryParsePercentByte(text, i, b0) || b0 < 0x80) {
      decoded += text[i++];
      continue;
    }
    uint8_t b1 = 0, b2 = 0, b3 = 0;
    const auto is_cont = [](uint8_t b) { return b >= 0x80 && b <= 0xBF; };
    if (b0 >= 0xC2 && b0 <= 0xDF && TryParsePercentByte(text, i + 3, b1) &&
        is_cont(b1)) {
      decoded += static_cast<char>(b0);
      decoded += static_cast<char>(b1);
      i += 6;
    } else if (TryParsePercentByte(text, i + 3, b1) &&
               TryParsePercentByte(text, i + 6, b2) && is_cont(b2) &&
               ((b0 == 0xE0 && b1 >= 0xA0 && b1 <= 0xBF) ||
                (((b0 >= 0xE1 && b0 <= 0xEC) || (b0 >= 0xEE && b0 <= 0xEF)) &&
                 is_cont(b1)) ||
                (b0 == 0xED && b1 >= 0x80 && b1 <= 0x9F))) {
      decoded += static_cast<char>(b0);
      decoded += static_cast<char>(b1);
      decoded += static_cast<char>(b2);
      i += 9;
    } else if (TryParsePercentByte(text, i + 3, b1) &&
               TryParsePercentByte(text, i + 6, b2) && is_cont(b2) &&
               TryParsePercentByte(text, i + 9, b3) && is_cont(b3) &&
               ((b0 == 0xF0 && b1 >= 0x90 && b1 <= 0xBF) ||
                (b0 >= 0xF1 && b0 <= 0xF3 && is_cont(b1)) ||
                (b0 == 0xF4 && b1 >= 0x80 && b1 <= 0x8F))) {
      decoded += static_cast<char>(b0);
      decoded += static_cast<char>(b1);
      decoded += static_cast<char>(b2);
      decoded += static_cast<char>(b3);
      i += 12;
    } else {
      decoded += text[i++];
    }
  }
  return decoded;
}

// Normalizes file:// URIs (both local file:/// and UNC file://host/) so
// WinRT resolves non-ASCII paths and reserved characters, while preserving
// percent-encoded UTF-8 sequences on network URLs.
inline std::string PrepareNativeSourceUri(const std::string& uri) {
  if (StartsWithIgnoreCase(uri, "file://")) {
    return EncodeSpacesInUri(DecodeUtf8PercentEscapes(uri));
  }
  return EncodeSpacesInUri(uri);
}

// Returns true iff |uri| is a plain HTTP URI whose authority strictly targets
// the local loopback interface (127.0.0.1, [::1], or localhost). Used to accept
// requests routed through just_audio's _ProxyHttpServer while continuing to
// reject direct custom HTTP headers on external URLs.
inline bool IsLoopbackHttpUri(const std::string& uri) {
  constexpr char kHttpScheme[] = "http://";
  constexpr size_t kSchemeLen = sizeof(kHttpScheme) - 1;
  if (!StartsWithIgnoreCase(uri, kHttpScheme)) return false;

  const size_t auth_start = kSchemeLen;
  size_t auth_end = uri.find_first_of("/?#", auth_start);
  if (auth_end == std::string::npos) auth_end = uri.length();
  if (auth_end <= auth_start) return false;

  const std::string authority = uri.substr(auth_start, auth_end - auth_start);
  if (authority.find('@') != std::string::npos) return false;

  std::string host;
  std::string port;
  bool has_port = false;
  if (authority.front() == '[') {
    const size_t close = authority.find(']');
    if (close == std::string::npos) return false;
    host = authority.substr(0, close + 1);
    if (close + 1 < authority.length()) {
      if (authority[close + 1] != ':') return false;
      has_port = true;
      port = authority.substr(close + 2);
    }
  } else {
    const size_t colon = authority.find(':');
    if (colon != std::string::npos) {
      has_port = true;
      host = authority.substr(0, colon);
      port = authority.substr(colon + 1);
    } else {
      host = authority;
    }
  }

  if (has_port) {
    if (port.empty() || port.length() > 5) return false;
    int port_num = 0;
    for (char c : port) {
      if (!std::isdigit(static_cast<unsigned char>(c))) return false;
      port_num = port_num * 10 + (c - '0');
    }
    if (port_num < 1 || port_num > 65535) return false;
  }

  return host == "127.0.0.1" || host == "[::1]" ||
         EqualsIgnoreCase(host, "localhost");
}
