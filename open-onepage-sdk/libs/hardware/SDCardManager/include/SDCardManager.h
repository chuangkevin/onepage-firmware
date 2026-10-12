#pragma once

#include <WString.h>
#include <vector>
#include <string>
#include <SdFat.h>

#include "FlashBlockDevice.h"

class SDCardManager {
 public:
  SDCardManager();
  bool begin();
  bool ready() const;
  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200);
  // Read the entire file at `path` into a String. Returns empty string on failure.
  String readFile(const char* path);
  // Low-memory helpers:
  // Stream the file contents to a `Print` (e.g. `Serial`, or any `Print`-derived object).
  // Returns true on success, false on failure.
  bool readFileToStream(const char* path, Print& out, size_t chunkSize = 256);
  // Read up to `bufferSize-1` bytes into `buffer`, null-terminating it. Returns bytes read.
  size_t readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes = 0);
  // Write a string to `path`. Overwrites existing file. Returns true on success.
  bool writeFile(const char* path, const String& content);
  // Ensure a directory exists, creating it if necessary. Returns true on success.
  bool ensureDirectoryExists(const char* path);

  FsFile open(const char* path, const oflag_t oflag = O_RDONLY) { return vol().open(path, oflag); }
  bool mkdir(const char* path, const bool pFlag = true) { return vol().mkdir(path, pFlag); }
  bool exists(const char* path) { return vol().exists(path); }
  bool remove(const char* path) { return vol().remove(path); }
  bool rmdir(const char* path) { return vol().rmdir(path); }
  bool rename(const char* oldPath, const char* newPath) { return vol().rename(oldPath, newPath); }

  bool openFileForRead(const char* moduleName, const char* path, FsFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, FsFile& file);
  bool openFileForRead(const char* moduleName, const String& path, FsFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, FsFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, FsFile& file);
  bool openFileForWrite(const char* moduleName, const String& path, FsFile& file);
  bool removeDir(const char* path);

  // True when no microSD was found and the internal-flash volume is in use.
  bool usingInternalStorage() const { return usingInternal; }

  // One-shot boot diagnostics for the internal-flash volume: write a file, read
  // it back and compare, then list the root. Records a marker so it runs once.
  void runBootSelfTest();

  static SDCardManager& getInstance() { return instance; }

 private:
  static SDCardManager instance;

  // One FAT-volume seam so every call site is backend-agnostic.
  FsVolume& vol() { return usingInternal ? static_cast<FsVolume&>(internalVol) : static_cast<FsVolume&>(sd); }

  bool beginInternal();
  void switchToInternal();

  bool initialized = false;
  bool usingInternal = false;
  SdFat sd;
  FsVolume internalVol;
  FlashBlockDevice internalDev;
};

#define SdMan SDCardManager::getInstance()
