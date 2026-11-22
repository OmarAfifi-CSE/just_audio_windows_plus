#pragma once

#include <string>

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
// turned into the text it represents. An invalid or truncated escape is kept
// literally, so malformed input is never silently rewritten.
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
    decoded += static_cast<char>((high << 4) | low);
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
