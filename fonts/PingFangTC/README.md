# PingFang TC (蘋方-繁) — SD-card fonts

Converted from the macOS built-in **PingFang.ttc** (face *PingFang TC Regular*)
into CrossPoint `.cpfont` (v4) with `lib/EpdFont/scripts/fontconvert_sdcard.py`.

| file | purpose | place on SD card as |
|---|---|---|
| `PingFangTC_10.cpfont` | UI / menu font (CJK glyph-miss fallback) | `/.fonts/LXGWWenKai-Regular/LXGWWenKai-Regular_12.cpfont` |
| `PingFangTC_12.cpfont` | reader font family | `/.fonts/PingFangTC/PingFangTC_12.cpfont` |
| `PingFangTC_14.cpfont` | reader font family | `/.fonts/PingFangTC/PingFangTC_14.cpfont` |
| `PingFangTC_16.cpfont` | reader font family | `/.fonts/PingFangTC/PingFangTC_16.cpfont` |
| `PingFangTC_18.cpfont` | reader font family | `/.fonts/PingFangTC/PingFangTC_18.cpfont` |

## Notes

- The firmware's CJK UI fallback path is **hardcoded**
  (`CjkUiFontSystem::kCandidatePaths`), so the UI font must live at
  `/.fonts/LXGWWenKai-Regular/LXGWWenKai-Regular_12.cpfont` even though the
  contents are PingFang. The `_10` raster is used because its metrics match the
  original LXGW build (advanceY 29 vs 30), so Chinese stays aligned with the
  built-in Latin UI font.
- Reader family (12/14/16/18) is picked via `Settings → Reader → Font Family`.

## Regenerate

```bash
# 1) extract PingFang TC Regular (face index 2) from the system TTC
python3 - <<'PY'
from fontTools.ttLib import TTCollection
c = TTCollection("/System/Library/AssetsV2/com_apple_MobileAsset_Font8/*/*/AssetData/PingFang.ttc")
c.fonts[2].save("PingFangTC-Regular.otf")
PY

# 2) UI font (size 10 ≈ LXGW metrics)
python3 lib/EpdFont/scripts/fontconvert_sdcard.py PingFangTC-Regular.otf \
  --intervals reading,cjk --size 10 --style regular \
  -o PingFangTC_10.cpfont

# 3) reader family
python3 lib/EpdFont/scripts/fontconvert_sdcard.py PingFangTC-Regular.otf \
  --intervals reading,cjk --sizes 12,14,16,18 --style regular \
  --name PingFangTC --output-dir .
```

## License

PingFang is Apple's proprietary system font. These converted files are for
personal use on the OnePage reader and are kept in this **private** repository
only — do not redistribute.
