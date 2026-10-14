#include "SimplifiedToTraditional.h"

#include <cstdint>
#include <cstring>

#include "LayoutMode.h"
#include "S2TTables.h"

namespace {

constexpr size_t kCharCount = sizeof(S2T::kChars) / sizeof(S2T::kChars[0]);
constexpr size_t kPhraseCount = sizeof(S2T::kPhrases) / sizeof(S2T::kPhrases[0]);

// Traditional form of one codepoint, or 0 when it is not a simplified character.
uint16_t mapChar(const uint32_t cp) {
  if (cp > 0xFFFF) return 0;
  size_t lo = 0, hi = kCharCount;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const uint16_t key = S2T::kChars[mid][0];
    if (key == cp) return S2T::kChars[mid][1];
    if (key < cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return 0;
}

void appendCodepoint(std::string& out, const uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// Index of the first phrase whose key is `cp`, or kPhraseCount when there is none.
size_t firstPhraseFor(const uint32_t cp) {
  if (cp > 0xFFFF) return kPhraseCount;
  size_t lo = 0, hi = kPhraseCount;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (S2T::kPhrases[mid].first < cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

}  // namespace

namespace SimplifiedToTraditional {

std::string convert(const std::string& in) {
  if (in.empty()) return in;
  // Traditional forms are usually the same byte length or one byte longer.
  std::string out;
  out.reserve(in.size() + in.size() / 8);

  size_t i = 0;
  while (i < in.size()) {
    size_t len = EpubLayout::utf8SeqLen(static_cast<uint8_t>(in[i]));
    if (i + len > in.size()) len = 1;
    const uint32_t cp = EpubLayout::utf8FirstCodepoint(in.substr(i, len));

    // Phrases first: the index is sorted by key and, within a key, longest first,
    // so the first byte-match in the run is the longest phrase that applies.
    bool matched = false;
    for (size_t k = firstPhraseFor(cp); k < kPhraseCount && S2T::kPhrases[k].first == cp; ++k) {
      const S2T::Phrase& ph = S2T::kPhrases[k];
      if (i + ph.srcLen > in.size()) continue;
      if (std::memcmp(in.data() + i, S2T::kBlob + ph.srcOff, ph.srcLen) != 0) continue;
      out.append(S2T::kBlob + ph.dstOff, ph.dstLen);
      i += ph.srcLen;
      matched = true;
      break;
    }
    if (matched) continue;

    const uint16_t mapped = mapChar(cp);
    if (mapped != 0) {
      appendCodepoint(out, mapped);
    } else {
      out.append(in, i, len);
    }
    i += len;
  }
  return out;
}

}  // namespace SimplifiedToTraditional
