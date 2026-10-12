#include "ReadflixActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/TailscaleManager.h"
#include "util/StringUtils.h"

namespace {
constexpr const char* READFLIX_BASE_URL = "https://readflix.sisihome.org";
constexpr const char* READFLIX_BOOKS_URL = "https://readflix.sisihome.org/api/books";
constexpr const char* BOOKS_DIR = "/books";
// Bound the parsed list so a pathological server response cannot exhaust RAM.
constexpr int MAX_BOOKS = 200;
}  // namespace

ReadflixActivity::ReadflixActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Readflix", renderer, mappedInput) {}

// --- Lifecycle ---

void ReadflixActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void ReadflixActivity::onExit() {
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void ReadflixActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  // readflix is only reachable over the tailnet: bring Tailscale up now that
  // Wi-Fi is connected and before the first HTTP request. No-op without a
  // /.crosspoint/tailscale.json config.
  TailscaleManager::instance().ensureUp();

  {
    RenderLock lock(*this);
    state_ = LOADING;
  }
  requestUpdateAndWait();

  if (!fetchAndParseBooks()) {
    {
      RenderLock lock(*this);
      state_ = ERROR;
    }
    return;
  }

  {
    RenderLock lock(*this);
    state_ = BOOK_LIST;
    selectedIndex_ = 0;
  }
}

// --- Book list fetching ---

bool ReadflixActivity::fetchAndParseBooks() {
  std::string json;
  if (!HttpDownloader::fetchUrl(READFLIX_BOOKS_URL, json)) {
    LOG_ERR("READFLIX", "Failed to fetch book list from %s", READFLIX_BOOKS_URL);
    errorMessage_ = "無法取得書單";
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json);
  if (err) {
    LOG_ERR("READFLIX", "Book list parse error: %s", err.c_str());
    errorMessage_ = "書單格式錯誤";
    return false;
  }

  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull()) {
    LOG_ERR("READFLIX", "Book list is not a JSON array");
    errorMessage_ = "書單格式錯誤";
    return false;
  }

  books_.clear();
  books_.reserve(std::min<size_t>(arr.size(), MAX_BOOKS));

  for (JsonObject obj : arr) {
    if (static_cast<int>(books_.size()) >= MAX_BOOKS) break;

    // Only EPUB downloads are supported by the internal reader.
    const std::string format = obj["format"] | "";
    if (format != "epub") continue;

    Book book;
    book.id = obj["id"] | "";
    if (book.id.empty()) continue;
    book.title = obj["title"] | "";
    if (book.title.empty()) book.title = "未命名";
    book.author = obj["author"] | "";
    book.fileSize = obj["fileSize"].as<size_t>();
    books_.push_back(std::move(book));
  }

  LOG_DBG("READFLIX", "Book list loaded: %zu books", books_.size());
  return true;
}

// --- Download ---

std::string ReadflixActivity::buildDestinationPath(const Book& book) const {
  // Keep the title readable on device but strip path-hostile characters and
  // cap the byte length so the resulting path fits comfortably in buffers.
  std::string name = StringUtils::sanitizeFilename(book.title, 60);
  return std::string(BOOKS_DIR) + "/" + name + ".epub";
}

void ReadflixActivity::downloadSelected() {
  if (downloadingIndex_ < 0 || downloadingIndex_ >= static_cast<int>(books_.size())) {
    return;
  }
  const Book& book = books_[downloadingIndex_];

  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    fileProgress_ = 0;
    fileTotal_ = 0;
    cancelRequested_ = false;
  }
  requestUpdateAndWait();

  if (!Storage.ensureDirectoryExists(BOOKS_DIR)) {
    LOG_ERR("READFLIX", "Failed to create directory %s", BOOKS_DIR);
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "無法建立書本目錄";
    return;
  }

  const std::string destPath = buildDestinationPath(book);
  const std::string url = std::string(READFLIX_BASE_URL) + "/api/books/" + book.id + "/file";
  LOG_DBG("READFLIX", "Downloading: %s -> %s", url.c_str(), destPath.c_str());

  const auto result = HttpDownloader::downloadToFile(
      url, destPath,
      [this](size_t downloaded, size_t total) {
        fileProgress_ = downloaded;
        fileTotal_ = total;
        mappedInput.update();
        if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
            mappedInput.wasPressed(MappedInputManager::Button::Back)) {
          cancelRequested_ = true;
        }
        requestUpdate(true);
      },
      &cancelRequested_);

  if (result == HttpDownloader::ABORTED) {
    RenderLock lock(*this);
    state_ = BOOK_LIST;
    return;
  }

  if (result != HttpDownloader::OK) {
    LOG_ERR("READFLIX", "Download failed: %s (%d)", book.title.c_str(), result);
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "下載失敗：" + book.title;
    return;
  }

  LOG_DBG("READFLIX", "Downloaded %s (%zu bytes)", destPath.c_str(), fileTotal_);
  RenderLock lock(*this);
  state_ = COMPLETE;
}

// --- Input handling ---

void ReadflixActivity::loop() {
  if (state_ == BOOK_LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    const int listSize = static_cast<int>(books_.size());
    const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

    buttonNavigator_.onNextRelease([this, listSize] {
      selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
      requestUpdate();
    });

    buttonNavigator_.onPreviousRelease([this, listSize] {
      selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
      requestUpdate();
    });

    buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
      selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
      requestUpdate();
    });

    buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
      selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
      requestUpdate();
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && !books_.empty()) {
      downloadingIndex_ = selectedIndex_;
      downloadSelected();
      requestUpdateAndWait();
      return;
    }
  } else if (state_ == COMPLETE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        state_ = BOOK_LIST;
      }
      requestUpdate();
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        state_ = BOOK_LIST;
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (downloadingIndex_ >= 0 && downloadingIndex_ < static_cast<int>(books_.size())) {
        downloadSelected();
        requestUpdateAndWait();
        return;
      } else {
        {
          RenderLock lock(*this);
          state_ = BOOK_LIST;
        }
        requestUpdate();
      }
    }
  }
}

// --- Rendering ---

std::string ReadflixActivity::formatSize(size_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%zu B", bytes);
  }
  return buf;
}

void ReadflixActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Readflix 書庫");

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, "載入書單…");
  } else if (state_ == BOOK_LIST) {
    if (books_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, "沒有書");
      const auto labels = mappedInput.mapLabels("返回", "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawList(
          renderer,
          Rect{0, contentTop, pageWidth, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
          static_cast<int>(books_.size()), selectedIndex_,
          [this](int index) -> std::string { return books_[index].title; },
          [this](int index) -> std::string {
            const auto& book = books_[index];
            const std::string prefix = book.author.empty() ? "" : book.author + "  ";
            return prefix + formatSize(book.fileSize);
          });

      const auto labels = mappedInput.mapLabels("返回", "下載", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state_ == DOWNLOADING) {
    const std::string statusText =
        downloadingIndex_ >= 0 && downloadingIndex_ < static_cast<int>(books_.size())
            ? std::string("下載中 ") + books_[downloadingIndex_].title
            : std::string("下載中");
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, statusText.c_str());

    float progress = 0;
    if (fileTotal_ > 0) {
      progress = static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_);
    }

    const int barY = centerY + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(progress * 100), 100);

    const auto labels = mappedInput.mapLabels("返回", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, "已下載", true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels("返回", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, "下載失敗", true, EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str());
    }
    const auto labels = mappedInput.mapLabels("返回", "下載", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
