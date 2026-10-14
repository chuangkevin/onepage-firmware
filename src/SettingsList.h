#pragma once

#include <HalClock.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <SdCardFontRegistry.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/settings/SettingsActivity.h"

// Build the font family setting dynamically. When registry is non-null, SD card fonts
// are appended after the built-in fonts. Otherwise only built-in fonts are listed.
inline SettingInfo buildFontFamilySetting(const SdCardFontRegistry* registry) {
  // Built-in font labels (StrId)
  std::vector<StrId> enumValues = {StrId::STR_NOTO_SERIF};
  // Runtime string labels for SD card fonts
  std::vector<std::string> enumStringValues;

  // Reserve: first CrossPointSettings::BUILTIN_FONT_COUNT entries use StrId, rest use strings
  if (registry) {
    const auto& families = registry->getFamilies();
    enumStringValues.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(enumStringValues),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
  }

  // Capture the SD font count for the lambdas
  const int sdFontCount = static_cast<int>(enumStringValues.size());

  // Total option count = built-in + SD card families
  // For the combined enumStringValues: we need all entries as strings (built-in names + SD names)
  // The render code checks enumStringValues first, then enumValues. So we build enumStringValues
  // with all options when SD fonts are present.
  std::vector<std::string> allStringValues;
  if (sdFontCount > 0) {
    // Only Noto Serif ships (Noto Sans was dropped to save flash), so a single
    // built-in entry precedes the SD-card families.
    allStringValues.push_back(I18N.get(StrId::STR_NOTO_SERIF));
    allStringValues.insert(allStringValues.end(), enumStringValues.begin(), enumStringValues.end());
  }

  SettingInfo s;
  s.nameId = StrId::STR_FONT_FAMILY;
  s.type = SettingType::ENUM;
  s.enumValues = std::move(enumValues);
  s.enumStringValues = std::move(allStringValues);
  s.key = "fontFamily";
  s.category = StrId::STR_CAT_READER;

  // Capture registry families by copy for the lambdas
  std::vector<std::string> sdFamilyNames;
  if (registry) {
    const auto& families = registry->getFamilies();
    sdFamilyNames.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(sdFamilyNames),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
  }

  s.valueGetter = [sdFamilyNames]() -> uint8_t {
    // If an SD card font is selected, find its index
    if (SETTINGS.sdFontFamilyName[0] != '\0') {
      for (int i = 0; i < static_cast<int>(sdFamilyNames.size()); i++) {
        if (sdFamilyNames[i] == SETTINGS.sdFontFamilyName) {
          return static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i);
        }
      }
      // SD font name not found in registry — fall through to built-in
    }
    return SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily : 0;
  };

  s.valueSetter = [sdFamilyNames](uint8_t v) {
    if (v < CrossPointSettings::BUILTIN_FONT_COUNT) {
      SETTINGS.fontFamily = v;
      SETTINGS.sdFontFamilyName[0] = '\0';
    } else {
      int sdIdx = v - CrossPointSettings::BUILTIN_FONT_COUNT;
      if (sdIdx < static_cast<int>(sdFamilyNames.size())) {
        strncpy(SETTINGS.sdFontFamilyName, sdFamilyNames[sdIdx].c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
        SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      }
    }
  };

  return s;
}

// Build the UI (menu/interface) CJK font picker: one list of "Default" plus
// every installed SD font FILE ("<Family> <size>"). Picking one sets both the
// family and the point size so a size change always has a target to load.
inline SettingInfo buildUiFontSetting(const SdCardFontRegistry* registry) {
  struct Pick {
    std::string family;
    uint8_t pt;
  };
  std::vector<std::string> labels;
  std::vector<Pick> picks;
  labels.push_back(I18N.get(StrId::STR_UI_FONT_DEFAULT));  // index 0 = default path
  if (registry) {
    for (const auto& f : registry->getFamilies()) {
      for (const auto& file : f.files) {
        labels.push_back(f.name + " " + std::to_string(file.pointSize));
        picks.push_back({f.name, file.pointSize});
      }
    }
  }

  SettingInfo s;
  s.nameId = StrId::STR_UI_FONT;
  s.type = SettingType::ENUM;
  s.enumStringValues = std::move(labels);
  s.key = "uiFontFamily";
  s.category = StrId::STR_CAT_DISPLAY;

  s.valueGetter = [picks]() -> uint8_t {
    if (SETTINGS.uiFontFamilyName[0] != '\0') {
      for (int i = 0; i < static_cast<int>(picks.size()); i++) {
        if (picks[i].family == SETTINGS.uiFontFamilyName && picks[i].pt == SETTINGS.uiFontSize) {
          return static_cast<uint8_t>(i + 1);
        }
      }
    }
    return 0;
  };
  s.valueSetter = [picks](uint8_t v) {
    if (v == 0 || v - 1 >= static_cast<int>(picks.size())) {
      SETTINGS.uiFontFamilyName[0] = '\0';
    } else {
      const Pick& p = picks[v - 1];
      strncpy(SETTINGS.uiFontFamilyName, p.family.c_str(), sizeof(SETTINGS.uiFontFamilyName) - 1);
      SETTINGS.uiFontFamilyName[sizeof(SETTINGS.uiFontFamilyName) - 1] = '\0';
      SETTINGS.uiFontSize = p.pt;
    }
  };
  return s;
}

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
//
// The static list is constructed exactly once (master's optimization, #1086 +
// #1636) so the per-entry SettingInfo cost is paid once. When an
// SdCardFontRegistry is supplied AND has SD card fonts installed, the
// font-family entry is replaced in a per-call copy with a registry-aware
// version. Callers without SD fonts pay only a vector copy.
inline std::vector<SettingInfo> getSettingsList(const SdCardFontRegistry* registry = nullptr) {
  static const std::vector<SettingInfo> baseList = [] {
    std::vector<SettingInfo> v = {
        // --- Display ---
        SettingInfo::Enum(StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen,
                          {StrId::STR_DARK, StrId::STR_LIGHT, StrId::STR_CUSTOM, StrId::STR_COVER, StrId::STR_NONE_OPT,
                           StrId::STR_COVER_CUSTOM, StrId::STR_QUICK_RESUME, StrId::STR_NAMEPLATE},
                          "sleepScreen", StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(StrId::STR_ORIENTATION, &CrossPointSettings::nameplateOrientation,
                          {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED,
                           StrId::STR_LANDSCAPE_CCW},
                          "nameplateOrientation", StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                          {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                          {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED},
                          "sleepScreenCoverFilter", StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(StrId::STR_QUICK_RESUME_TIMEOUT, &CrossPointSettings::quickResumeSleepScreen,
                          {StrId::STR_STATE_OFF, StrId::STR_STATE_ON}, "quickResumeSleepScreen",
                          StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                          {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideBatteryPercentage",
                          StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(
            StrId::STR_REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
            {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15, StrId::STR_PAGES_30},
            "refreshFrequency", StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(StrId::STR_UI_THEME, &CrossPointSettings::uiTheme,
                          {StrId::STR_THEME_CLASSIC, StrId::STR_THEME_LYRA, StrId::STR_THEME_LYRA_EXTENDED,
                           StrId::STR_THEME_ROUNDEDRAFF},
                          "uiTheme", StrId::STR_CAT_DISPLAY),
        // "Sunlight fading fix" is intentionally not exposed: this firmware's
        // GfxRenderer was written against an SDK that had a fadingFix parameter,
        // but this SDK's displayBuffer/displayGrayscaleBase/displayGrayBuffer
        // take turnOffScreen instead. Toggling it only powered the panel down
        // after every frame. Left in CrossPointSettings for settings-file
        // compatibility, but no longer surfaced.
        // (UI font family + size is inserted dynamically in getSettingsList()
        //  as a single picker; see buildUiFontSetting().)

        // --- Reader ---
        // Built-in font-family entry. Replaced per-call with a registry-aware
        // version when SD fonts are installed.
        SettingInfo::Enum(StrId::STR_FONT_FAMILY, &CrossPointSettings::fontFamily,
                          {StrId::STR_NOTO_SERIF}, "fontFamily", StrId::STR_CAT_READER),
        SettingInfo::Enum(StrId::STR_FONT_SIZE, &CrossPointSettings::fontSize,
                          {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE, StrId::STR_X_LARGE}, "fontSize",
                          StrId::STR_CAT_READER),
        SettingInfo::Enum(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacing,
                          {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE}, "lineSpacing", StrId::STR_CAT_READER),
        SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMargin, {5, 40, 5}, "screenMargin",
                           StrId::STR_CAT_READER),
        SettingInfo::Enum(StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
                          {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                           StrId::STR_BOOK_S_STYLE},
                          "paragraphAlignment", StrId::STR_CAT_READER),
        SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle, "embeddedStyle",
                            StrId::STR_CAT_READER),
        SettingInfo::Toggle(StrId::STR_FOCUS_READING, &CrossPointSettings::focusReadingEnabled, "focusReadingEnabled",
                            StrId::STR_CAT_READER),
        SettingInfo::Toggle(StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled, "hyphenationEnabled",
                            StrId::STR_CAT_READER),
        // Vertical CJK writing: only affects EPUB page layout.
        SettingInfo::Toggle(StrId::STR_VERTICAL_TEXT, &CrossPointSettings::verticalText, "verticalText",
                            StrId::STR_CAT_READER),
        // Simplified -> Traditional rewrite of the book text during layout.
        SettingInfo::Toggle(StrId::STR_CONVERT_TRADITIONAL, &CrossPointSettings::convertToTraditional,
                            "convertToTraditional", StrId::STR_CAT_READER),
        SettingInfo::Enum(
            StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
            {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW},
            "orientation", StrId::STR_CAT_READER),
        SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing,
                            "extraParagraphSpacing", StrId::STR_CAT_READER),
        SettingInfo::Toggle(StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing, "textAntiAliasing",
                            StrId::STR_CAT_READER),
        SettingInfo::Toggle(StrId::STR_RULED_LINES, &CrossPointSettings::ruledLines, "ruledLines",
                            StrId::STR_CAT_READER),
        SettingInfo::Enum(StrId::STR_RULED_LINE_STYLE, &CrossPointSettings::ruledLineStyle,
                          {StrId::STR_RULED_DASHED, StrId::STR_RULED_SOLID, StrId::STR_RULED_DOTTED,
                           StrId::STR_RULED_DASH_DOT, StrId::STR_RULED_ZIGZAG},
                          "ruledLineStyle", StrId::STR_CAT_READER),
        SettingInfo::Value(StrId::STR_RULED_LINE_OFFSET, &CrossPointSettings::ruledLineOffset, {0, 20, 1},
                           "ruledLineOffset", StrId::STR_CAT_READER),
        SettingInfo::Enum(StrId::STR_RULED_LINE_THICKNESS, &CrossPointSettings::ruledLineThickness,
                          {StrId::STR_1PX, StrId::STR_2PX, StrId::STR_3PX}, "ruledLineThickness",
                          StrId::STR_CAT_READER),
        SettingInfo::Enum(StrId::STR_RULED_LINE_DASH, &CrossPointSettings::ruledLineDash,
                          {StrId::STR_DASH_SPARSE, StrId::STR_DASH_NORMAL, StrId::STR_DASH_DENSE}, "ruledLineDash",
                          StrId::STR_CAT_READER),
        SettingInfo::Enum(StrId::STR_IMAGES, &CrossPointSettings::imageRendering,
                          {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                          "imageRendering", StrId::STR_CAT_READER),
        // --- Controls ---
        SettingInfo::Enum(StrId::STR_SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
                          {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV, StrId::STR_DISABLED}, "sideButtonLayout",
                          StrId::STR_CAT_CONTROLS),
        SettingInfo::Toggle(StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION, &CrossPointSettings::frontButtonFollowOrientation,
                            "frontButtonFollowOrientation", StrId::STR_CAT_CONTROLS),
        SettingInfo::Enum(StrId::STR_LONG_PRESS_BEHAVIOR, &CrossPointSettings::longPressButtonBehavior,
                          {StrId::STR_LONG_PRESS_BEHAVIOR_OFF, StrId::STR_LONG_PRESS_BEHAVIOR_SKIP,
                           StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION},
                          "longPressButtonBehavior", StrId::STR_CAT_CONTROLS),
        SettingInfo::Enum(StrId::STR_LONG_PRESS_MENU, &CrossPointSettings::longPressMenuFunction,
                          {StrId::STR_KOSYNC, StrId::STR_DISABLED, StrId::STR_BOOKMARK_OPTION}, "longPressMenuFunction",
                          StrId::STR_CAT_CONTROLS),
        SettingInfo::Enum(
            StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn,
            {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_PAGE_TURN, StrId::STR_FORCE_REFRESH, StrId::STR_FOOTNOTES},
            "shortPwrBtn", StrId::STR_CAT_CONTROLS),
        SettingInfo::Toggle(StrId::STR_PWR_BTN_FOOTNOTE_BACK, &CrossPointSettings::pwrBtnFootnoteBack,
                            "pwrBtnFootnoteBack", StrId::STR_CAT_CONTROLS),

        // --- System ---
        SettingInfo::Value(
            StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeoutMinutes,
            {CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1},
            "sleepTimeoutMinutes", StrId::STR_CAT_SYSTEM),
        SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &CrossPointSettings::showHiddenFiles, "showHiddenFiles",
                            StrId::STR_CAT_SYSTEM),
        SettingInfo::Toggle(StrId::STR_REMOVE_READ_FROM_RECENTS, &CrossPointSettings::removeReadBooksFromRecents,
                            "removeReadBooksFromRecents", StrId::STR_CAT_SYSTEM),
        SettingInfo::Toggle(StrId::STR_MOVE_FINISHED_TO_READ, &CrossPointSettings::moveFinishedToReadFolder,
                            "moveFinishedToReadFolder", StrId::STR_CAT_SYSTEM),

        // --- Status Bar Settings (web-only, uses StatusBarSettingsActivity) ---
        SettingInfo::Toggle(StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarChapterPageCount,
                            "statusBarChapterPageCount", StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Toggle(StrId::STR_BOOK_PROGRESS_PERCENTAGE, &CrossPointSettings::statusBarBookProgressPercentage,
                            "statusBarBookProgressPercentage", StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(StrId::STR_PROGRESS_BAR, &CrossPointSettings::statusBarProgressBar,
                          {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarProgressBar",
                          StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarProgressBarThickness,
                          {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                          "statusBarProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(StrId::STR_TITLE, &CrossPointSettings::statusBarTitle,
                          {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarTitle",
                          StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Toggle(StrId::STR_BATTERY, &CrossPointSettings::statusBarBattery, "statusBarBattery",
                            StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(StrId::STR_XTC_STATUS_BAR, &CrossPointSettings::xtcStatusBarMode,
                          {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP}, "xtcStatusBarMode",
                          StrId::STR_CUSTOMISE_STATUS_BAR),
        // Clock entries (web settings only; device UI uses ClockOffsetActivity for the offset).
        // Range 0..104 = quarter-hour steps from UTC-12:00 to UTC+14:00, biased by 48.
        SettingInfo::Enum(StrId::STR_CLOCK, &CrossPointSettings::statusBarClock,
                          {StrId::STR_HIDE, StrId::STR_DIR_LEFT, StrId::STR_DIR_RIGHT}, "statusBarClock",
                          StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Value(StrId::STR_CLOCK_UTC_OFFSET, &CrossPointSettings::clockUtcOffsetQ, {0, 104, 1},
                           "clockUtcOffsetQ", StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(StrId::STR_CLOCK_FORMAT, &CrossPointSettings::clockFormat,
                          {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H}, "clockFormat",
                          StrId::STR_CUSTOMISE_STATUS_BAR),
        // Persistence flag for NTP debounce. Resetting from the web UI forces a re-sync
        // on next WiFi connect, which is useful when crossing time zones.
        SettingInfo::Toggle(StrId::STR_CLOCK_SYNCED, &CrossPointSettings::clockHasBeenSynced, "clockHasBeenSynced",
                            StrId::STR_CUSTOMISE_STATUS_BAR),
    };
    // Only show tilt page turn setting when the QMI8658 IMU is present (X3)
    if (halTiltSensor.isAvailable()) {
      // Insert after the short power button setting (end of Controls section)
      for (auto it = v.begin(); it != v.end(); ++it) {
        if (it->nameId == StrId::STR_SHORT_PWR_BTN) {
          v.insert(it + 1, SettingInfo::Enum(StrId::STR_TILT_PAGE_TURN, &CrossPointSettings::tiltPageTurn,
                                             {StrId::STR_STATE_OFF, StrId::STR_NORMAL, StrId::STR_INVERTED},
                                             "tiltPageTurn", StrId::STR_CAT_CONTROLS));
          break;
        }
      }
    }
    return v;
  }();

  std::vector<SettingInfo> v = baseList;
  if (registry && registry->getFamilyCount() > 0) {
    auto it = std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_FONT_FAMILY; });
    if (it != v.end()) {
      *it = buildFontFamilySetting(registry);
    }
  }
  // Insert the UI (menu/interface) font picker (family + size) at the end of
  // the Display section. Always present (shows at least "Default").
  auto itd =
      std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_SUNLIGHT_FADING_FIX; });
  if (itd != v.end()) {
    v.insert(itd + 1, buildUiFontSetting(registry));
  }
  return v;
}
