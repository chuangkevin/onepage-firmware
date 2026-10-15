#include "TextBlock.h"

#include <BidiUtils.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstring>

#include "../LayoutMode.h"

// Encodes one codepoint as UTF-8 into buf; returns the byte count (0 if invalid).
static int encodeUtf8(const uint32_t cp, char* buf) {
  if (cp < 0x80) { buf[0] = static_cast<char>(cp); return 1; }
  if (cp < 0x800) { buf[0] = static_cast<char>(0xC0 | (cp >> 6)); buf[1] = static_cast<char>(0x80 | (cp & 0x3F)); return 2; }
  buf[0] = static_cast<char>(0xE0 | (cp >> 12));
  buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
  return 3;
}

// Sentence punctuation. Taiwanese (教育部) vertical typesetting centres these in the
// character cell. Brackets (「」『』（）【】《》) are deliberately NOT in this set: they
// are handled separately (rotated, then nudged to a cell corner).
static bool isVerticalSentencePunctuation(const uint32_t cp) {
  switch (cp) {
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
    case 0xFF1B:  // ；
    case 0xFF1A:  // ：
    case 0xFF01:  // ！
    case 0xFF1F:  // ？
      return true;
    default:
      return false;
  }
}

// The font's own glyph for vertical setting. Using these beats shifting the
// horizontal glyph by a fraction of a cell, which is only ever an approximation and
// is what made vertical punctuation look wrong. Zero means "no vertical form".
static uint32_t verticalFormOf(const uint32_t cp) {
  switch (cp) {
    case 0x3001: return 0xFE11;  // 、
    case 0x3002: return 0xFE12;  // 。
    case 0xFF0C: return 0xFE10;  // ，
    case 0xFF1A: return 0xFE13;  // ：
    case 0xFF1B: return 0xFE14;  // ；
    case 0xFF01: return 0xFE15;  // ！
    case 0xFF1F: return 0xFE16;  // ？
    case 0x300C: return 0xFE41;  // 「
    case 0x300D: return 0xFE42;  // 」
    case 0x300E: return 0xFE43;  // 『
    case 0x300F: return 0xFE44;  // 』
    default: return 0;
  }
}

// Characters whose horizontal glyph, rotated 90 degrees clockwise, IS the correct
// vertical-setting glyph (it matches the font's own vertical presentation form, e.g.
// 「 U+300C rotated == FE41). Used only when the font has no vertical form, because
// otherwise the horizontal glyph would sit un-rotated in the middle of the cell and
// read as broken - the previous behaviour for brackets.
static bool isVerticalRotatedPunctuation(const uint32_t cp) {
  switch (cp) {
    case 0x300C: case 0x300D:  // 「 」
    case 0x300E: case 0x300F:  // 『 』
    case 0x3008: case 0x3009:  // 〈 〉
    case 0x300A: case 0x300B:  // 《 》
    case 0x3010: case 0x3011:  // 【 】
    case 0x3014: case 0x3015:  // 〔 〕
    case 0x3016: case 0x3017:  // 〖 〗
    case 0xFF08: case 0xFF09:  // （ ）
    case 0xFF3B: case 0xFF3D:  // ［ ］
    case 0xFF5B: case 0xFF5D:  // ｛ ｝
    case 0x2014: case 0x2015:  // — ―
    case 0xFF0D:               // －
    case 0x2026: case 0x2025:  // … ‥
    case 0x2018: case 0x2019:  // ' '
    case 0x201C: case 0x201D:  // " "
      return true;
    default:
      return false;
  }
}

// Opening vs closing brackets/quotes. Taiwanese (教育部) vertical typesetting hugs the
// quoted text: the opening mark sits at the BOTTOM-LEFT of its cell and the closing mark
// at the TOP-RIGHT, so the pair brackets the characters between them.
static bool isVerticalOpeningBracket(const uint32_t cp) {
  switch (cp) {
    case 0x300C: case 0x300E: case 0x3008: case 0x300A:  // 「 『 〈 《
    case 0x3010: case 0x3014: case 0x3016:               // 【 〔 〖
    case 0xFF08: case 0xFF3B: case 0xFF5B:               // （ ［ ｛
    case 0x2018: case 0x201C:                            // ' "
      return true;
    default:
      return false;
  }
}

static bool isVerticalClosingBracket(const uint32_t cp) {
  switch (cp) {
    case 0x300D: case 0x300F: case 0x3009: case 0x300B:  // 」 』 〉 》
    case 0x3011: case 0x3015: case 0x3017:               // 】 〕 〗
    case 0xFF09: case 0xFF3D: case 0xFF5D:               // ） ］ ｝
    case 0x2019: case 0x201D:                            // ' "
      return true;
    default:
      return false;
  }
}

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  // Focus annotations are optional: empty vectors mean no word in this block has a split.
  // When present, they must be sized in lockstep with words[].
  const bool hasFocus = !wordFocusBoundary.empty();
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() ||
      (hasFocus && (words.size() != wordFocusBoundary.size() || words.size() != wordFocusSuffixX.size()))) {
    LOG_ERR("TXB", "Render skipped: size mismatch (words=%u, xpos=%u, styles=%u, boundary=%u, suffixX=%u)\n",
            (uint32_t)words.size(), (uint32_t)wordXpos.size(), (uint32_t)wordStyles.size(),
            (uint32_t)wordFocusBoundary.size(), (uint32_t)wordFocusSuffixX.size());
    return;
  }

  const bool scanning = renderer.isFontCacheScanning();
  const int ascender = renderer.getFontAscenderSize(fontId);

  if (EpubLayout::vertical) {
    // Vertical CJK: the glyphs stay upright and stack down the column, so this
    // cannot be a rotated draw. wordXpos (placement across a line) is meaningless
    // here; the column x is the block origin and every codepoint advances one
    // line height downwards.
    //
    // Two typographic rules make it read properly:
    //  - each glyph is centred across the column instead of hugging its left edge
    //    (CJK advances are a full em, so this only matters for digits/latin);
    //  - sentence punctuation and closing brackets move to the top-right of the
    //    cell, which is where vertical Chinese typesetting puts them.
    const int lineHeight = renderer.getLineHeight(fontId);
    int down = 0;
    for (size_t i = 0; i < words.size(); i++) {
      const std::string& word = words[i];
      const EpdFontFamily::Style verticalStyle = wordStyles[i];
      const auto baseDir = static_cast<BidiUtils::BidiBaseDir>(
          BidiUtils::detectParagraphLevel(word.c_str(), blockStyle.isRtl ? 1 : 0));
      for (size_t p = 0; p < word.size();) {
        const size_t len = EpubLayout::utf8SeqLen(static_cast<uint8_t>(word[p]));
        const size_t take = (p + len <= word.size()) ? len : 1;
        const std::string glyph = word.substr(p, take);
        p += take;

        const uint32_t cp = EpubLayout::utf8FirstCodepoint(glyph);
        std::string draw = glyph;
        int gx = x;
        int gy = y + down;
        bool placed = false;

        // Prefer the font's vertical form: its ink is already positioned for a
        // vertical cell, so it needs no shifting at all.
        if (const uint32_t vf = verticalFormOf(cp); vf != 0) {
          std::string vfGlyph;
          vfGlyph.resize(4);
          const int n = encodeUtf8(vf, vfGlyph.data());
          vfGlyph.resize(n > 0 ? n : 0);
          if (!vfGlyph.empty() && renderer.getTextWidth(fontId, vfGlyph.c_str(), verticalStyle, baseDir) > 0) {
            draw = vfGlyph;
            const int w = renderer.getTextWidth(fontId, draw.c_str(), verticalStyle, baseDir);
            gx = x + (lineHeight - w) / 2;
            placed = true;
          }
        }
        if (!placed) {
          if (isVerticalRotatedPunctuation(cp)) {
            // Brackets, dashes and the ellipsis have no useful pre-positioned form in
            // fonts without vertical glyphs: rotate the horizontal glyph 90 degrees CW
            // so it reads correctly down the column, matching the font's own vertical
            // form (e.g. 「 rotated == U+FE41).
            //
            // Taiwanese (教育部) style then hugs the quoted text: the opening mark sits
            // at the bottom-right of its cell, the closing mark at the top-left.
            const int nudge = lineHeight / 4;
            int cellX = x;
            int cellY = y + down;
            if (isVerticalOpeningBracket(cp)) {
              cellX += nudge;
              cellY += nudge;
            } else if (isVerticalClosingBracket(cp)) {
              cellX -= nudge;
              cellY -= nudge;
            }
            renderer.drawGlyphRotated90CW(fontId, cellX, cellY, lineHeight, glyph.c_str(), true, verticalStyle);
            placed = true;
          } else if (isVerticalSentencePunctuation(cp)) {
            // Taiwanese style centres sentence punctuation in the cell. Centre by ink
            // (not by advance): the glyph ink sits low-left in the em box, so a
            // half-cell nudge would still leave it off-centre.
            renderer.drawGlyphCentered(fontId, x, y + down, lineHeight, glyph.c_str(), true, verticalStyle);
            placed = true;
          } else {
            const int glyphWidth = renderer.getTextWidth(fontId, draw.c_str(), verticalStyle, baseDir);
            gx = x + (lineHeight - glyphWidth) / 2;
          }
        }
        if (!placed) {
          renderer.drawText(fontId, gx, gy, draw.c_str(), true, verticalStyle, baseDir);
        }
        down += lineHeight;
      }
    }
    return;
  }

  for (size_t i = 0; i < words.size(); i++) {
    const int wordX = wordXpos[i] + x;
    const EpdFontFamily::Style currentStyle = wordStyles[i];
    const auto baseDir = static_cast<BidiUtils::BidiBaseDir>(
        BidiUtils::detectParagraphLevel(words[i].c_str(), blockStyle.isRtl ? 1 : 0));
    const uint8_t boundary = hasFocus ? wordFocusBoundary[i] : 0;

    // SUP/SUB shift the baseline passed to drawText; the glyph is also scaled 50% inside
    // drawText, so these offsets are chosen relative to the full-size ascender:
    //   SUP: raise by 40% of ascender — sits clearly above the cap-height
    //   SUB: lower by 25% of ascender — descends below baseline without clashing with ascenders below
    int wordY = y;
    if ((currentStyle & EpdFontFamily::SUP) != 0) {
      wordY -= ascender * 2 / 5;
    } else if ((currentStyle & EpdFontFamily::SUB) != 0) {
      wordY += ascender / 4;
    }

    if (boundary > 0) {
      // Focus split: draw bold prefix, then the regular suffix at a pre-computed x offset.
      // The bold prefix is bounded to 9 codepoints by the clamp on targetBoldChars in
      // ParsedText::addWord; 9 UTF-8 codepoints occupy at most 9 * 4 = 36 bytes, +1 for null = 37.
      // suffixX is computed at cache-creation time to avoid font metric lookups at render time.
      static constexpr size_t MAX_FOCUS_PREFIX_BYTES = 9 * 4 + 1;
      char boldBuf[40];
      static_assert(sizeof(boldBuf) >= MAX_FOCUS_PREFIX_BYTES,
                    "boldBuf too small for max focus prefix (9 codepoints * 4 UTF-8 bytes + null)");
      const auto boldStyle = static_cast<EpdFontFamily::Style>(currentStyle | EpdFontFamily::BOLD);
      const size_t boldLen = std::min<size_t>({static_cast<size_t>(boundary), words[i].size(), sizeof(boldBuf) - 1});
      memcpy(boldBuf, words[i].c_str(), boldLen);
      boldBuf[boldLen] = '\0';
      renderer.drawText(fontId, wordX, wordY, boldBuf, true, boldStyle, baseDir);
      const int suffixX = wordX + wordFocusSuffixX[i];
      renderer.drawText(fontId, suffixX, wordY, words[i].c_str() + boldLen, true, currentStyle, baseDir);
    } else {
      renderer.drawText(fontId, wordX, wordY, words[i].c_str(), true, currentStyle, baseDir);
    }

    if (!scanning && (currentStyle & EpdFontFamily::UNDERLINE) != 0) {
      const std::string& w = words[i];
      int underlineWidth = renderer.getTextWidth(fontId, w.c_str(), currentStyle, baseDir);
      const int underlineY = wordY + ascender + 2;

      if ((currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
        underlineWidth = (underlineWidth + 1) / 2;
      }

      renderer.drawLine(wordX, underlineY, wordX + underlineWidth, underlineY, true);
    }
  }
}

bool TextBlock::serialize(HalFile& file) const {
  // Focus annotations are optional; vectors are either empty (no splits in this block)
  // or sized in lockstep with words[].
  const bool hasFocus = !wordFocusBoundary.empty();
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() ||
      (hasFocus && (words.size() != wordFocusBoundary.size() || words.size() != wordFocusSuffixX.size()))) {
    LOG_ERR("TXB", "Serialization failed: size mismatch (words=%u, xpos=%u, styles=%u, boundary=%u, suffixX=%u)\n",
            static_cast<uint32_t>(words.size()), static_cast<uint32_t>(wordXpos.size()),
            static_cast<uint32_t>(wordStyles.size()), static_cast<uint32_t>(wordFocusBoundary.size()),
            static_cast<uint32_t>(wordFocusSuffixX.size()));
    return false;
  }

  // Word data
  serialization::writePod(file, static_cast<uint16_t>(words.size()));
  for (const auto& w : words) serialization::writeString(file, w);
  for (auto x : wordXpos) serialization::writePod(file, x);
  for (auto s : wordStyles) serialization::writePod(file, s);
  // Focus block: 1-byte presence flag, followed by per-word vectors only when present.
  // Saves 3 bytes/word when focus reading is disabled or no word on this line was split.
  serialization::writePod(file, static_cast<uint8_t>(hasFocus ? 1 : 0));
  if (hasFocus) {
    for (auto b : wordFocusBoundary) serialization::writePod(file, b);
    for (auto sx : wordFocusSuffixX) serialization::writePod(file, sx);
  }

  // Style (alignment + margins/padding/indent)
  serialization::writePod(file, blockStyle.alignment);
  serialization::writePod(file, blockStyle.textAlignDefined);
  serialization::writePod(file, blockStyle.marginTop);
  serialization::writePod(file, blockStyle.marginBottom);
  serialization::writePod(file, blockStyle.marginLeft);
  serialization::writePod(file, blockStyle.marginRight);
  serialization::writePod(file, blockStyle.paddingTop);
  serialization::writePod(file, blockStyle.paddingBottom);
  serialization::writePod(file, blockStyle.paddingLeft);
  serialization::writePod(file, blockStyle.paddingRight);
  serialization::writePod(file, blockStyle.textIndent);
  serialization::writePod(file, blockStyle.textIndentDefined);
  serialization::writePod(file, blockStyle.isRtl);
  serialization::writePod(file, blockStyle.directionDefined);

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(HalFile& file) {
  uint16_t wc;
  std::vector<std::string> words;
  std::vector<int16_t> wordXpos;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<uint8_t> wordFocusBoundary;
  std::vector<uint16_t> wordFocusSuffixX;
  BlockStyle blockStyle;

  // Word count
  serialization::readPod(file, wc);

  // Sanity check: prevent allocation of unreasonably large vectors (max 10000 words per block)
  if (wc > 10000) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }

  // Word data
  words.resize(wc);
  wordXpos.resize(wc);
  wordStyles.resize(wc);
  for (auto& w : words) serialization::readString(file, w);
  for (auto& x : wordXpos) serialization::readPod(file, x);
  for (auto& s : wordStyles) serialization::readPod(file, s);
  // Focus block: presence flag, then vectors only if present. Empty vectors when absent
  // signal "no splits in this block" to render() (zero per-word RAM cost).
  uint8_t hasFocus;
  serialization::readPod(file, hasFocus);
  if (hasFocus) {
    wordFocusBoundary.resize(wc);
    wordFocusSuffixX.resize(wc);
    for (auto& b : wordFocusBoundary) serialization::readPod(file, b);
    for (auto& sx : wordFocusSuffixX) serialization::readPod(file, sx);
  }

  // Style (alignment + margins/padding/indent)
  serialization::readPod(file, blockStyle.alignment);
  serialization::readPod(file, blockStyle.textAlignDefined);
  serialization::readPod(file, blockStyle.marginTop);
  serialization::readPod(file, blockStyle.marginBottom);
  serialization::readPod(file, blockStyle.marginLeft);
  serialization::readPod(file, blockStyle.marginRight);
  serialization::readPod(file, blockStyle.paddingTop);
  serialization::readPod(file, blockStyle.paddingBottom);
  serialization::readPod(file, blockStyle.paddingLeft);
  serialization::readPod(file, blockStyle.paddingRight);
  serialization::readPod(file, blockStyle.textIndent);
  serialization::readPod(file, blockStyle.textIndentDefined);
  serialization::readPod(file, blockStyle.isRtl);
  serialization::readPod(file, blockStyle.directionDefined);

  return std::unique_ptr<TextBlock>(new TextBlock(std::move(words), std::move(wordXpos), std::move(wordStyles),
                                                  std::move(wordFocusBoundary), std::move(wordFocusSuffixX),
                                                  blockStyle));
}
