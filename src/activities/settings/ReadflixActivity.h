#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Browses and downloads EPUB books from the Readflix server onto internal
// storage ("/books/<name>.epub") so the book browser can open them.
class ReadflixActivity : public Activity {
 public:
  explicit ReadflixActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state_ == LOADING || state_ == DOWNLOADING ||
           // The download is synchronous and blocks the main loop until it
           // completes, so activityManager.preventAutoSleep() is never polled
           // during downloading.
           state_ == COMPLETE || state_ == ERROR;
  }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING,
    BOOK_LIST,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  struct Book {
    std::string id;
    std::string title;
    std::string author;
    size_t fileSize = 0;
  };

  State state_ = WIFI_SELECTION;
  ButtonNavigator buttonNavigator_;

  // Book list data
  std::vector<Book> books_;
  int selectedIndex_ = 0;

  // Download progress
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int downloadingIndex_ = -1;
  std::string errorMessage_;
  bool cancelRequested_ = false;

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseBooks();
  void downloadSelected();
  std::string buildDestinationPath(const Book& book) const;
  static std::string formatSize(size_t bytes);
};
