#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Reading direction for EPUB text.
//
// Vertical CJK writing is a per-device setting, but the line/word plumbing has no
// per-block notion of direction, and TextBlock/PageLine are serialized into the
// layout cache (adding a field there would invalidate every cached page). So the
// mode lives here as a process-wide flag, set once by the app when a book opens.
//
// In this mode a layout "line" becomes a COLUMN: characters run down the page and
// columns advance from the right edge leftwards. The line breaker is reused as-is
// by measuring each word along the column (one line height per codepoint) instead
// of across it.
namespace EpubLayout {
inline bool vertical = false;

// Byte length of the UTF-8 sequence starting at lead, clamped to what is left.
inline size_t utf8SeqLen(const uint8_t lead) {
  if ((lead & 0x80) == 0x00) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 1;  // continuation byte where a lead was expected: treat as its own char
}

// Number of codepoints in a UTF-8 string.
inline size_t utf8CodepointCount(const std::string& s) {
  size_t n = 0;
  for (size_t i = 0; i < s.size();) {
    const size_t len = utf8SeqLen(static_cast<uint8_t>(s[i]));
    i += (i + len <= s.size()) ? len : 1;
    ++n;
  }
  return n;
}
}  // namespace EpubLayout
