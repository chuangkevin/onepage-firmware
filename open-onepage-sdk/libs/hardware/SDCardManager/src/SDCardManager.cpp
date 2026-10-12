#include "SDCardManager.h"

#include <Preferences.h>
#include <esp_partition.h>

namespace {
#ifdef ONEPAGE_C61
constexpr uint8_t SD_CS = 26;        // OnePage SD CS (shared SPI bus with EPD)
// 20MHz. The shared EPD+SD bus was marginal at 20MHz (intermittent SD drops on
// heavy activity); instead of slowing the clock we lower the SPI pins' GPIO
// drive strength one level (CAP_2 -> CAP_1) in HalGPIO::begin to soften edges /
// cut ringing, which is the real signal-integrity fix.
constexpr uint32_t SPI_FQ = 20000000;
#else
constexpr uint8_t SD_CS = 12;
constexpr uint32_t SPI_FQ = 40000000;
#endif

// NVS flag: internal storage holds user data that has not been copied to a
// microSD yet. Set on every write while running on internal storage; cleared
// once the migration to SD has been verified.
constexpr const char* kNvsNamespace = "onepage";
constexpr const char* kNvsDirtyKey = "int_dirty";

bool nvsGetDirty() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, true)) return false;
  const bool dirty = prefs.getBool(kNvsDirtyKey, false);
  prefs.end();
  return dirty;
}

void nvsSetDirty(bool value) {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, false)) return;
  prefs.putBool(kNvsDirtyKey, value);
  prefs.end();
}
}  // namespace

SDCardManager SDCardManager::instance;

SDCardManager::SDCardManager() : sd() {}

bool SDCardManager::begin() {
#ifdef ONEPAGE_C61
  // The SD shares the GPIO27 power rail with the EPD; right after the rail
  // powers up the card sometimes isn't ready on the first sd.begin() (power-up /
  // SPI-init timing), causing an intermittent "SD card error" at boot. Retry a
  // few times with a short settle delay before giving up — a reboot always
  // worked, which is exactly the symptom of a too-early first attempt.
  initialized = false;
  bool sdOk = false;
  for (int attempt = 0; attempt < 5; ++attempt) {
    if (sd.begin(SD_CS, SPI_FQ)) {
      sdOk = true;
      break;
    }
    delay(100);
  }
  if (Serial) {
    Serial.printf("[%lu] [SD] %s\n", millis(), sdOk ? "SD card detected" : "SD card not detected");
  }
#else
  const bool sdOk = sd.begin(SD_CS, SPI_FQ);
  if (Serial) {
    Serial.printf("[%lu] [SD] %s\n", millis(), sdOk ? "SD card detected" : "SD card not detected");
  }
#endif

  if (sdOk) {
    usingInternal = false;
    initialized = true;
    // Was anything written while running on internal storage? If so, copy it
    // out to the card now (copy-only, verified).
    migrateInternalToSd();
    return true;
  }

  // No microSD: fall back to the internal-flash volume so the reader still runs.
  if (beginInternal()) {
    switchToInternal();
    initialized = true;
    if (Serial) Serial.printf("[%lu] [SD] using internal flash storage\n", millis());
    runBootSelfTest();
    return true;
  }

  initialized = false;
  return false;
}

void SDCardManager::switchToInternal() {
  // FsVolume::begin() already published this volume as the current working
  // volume, so callers that use SdFat's global seam see it too. All our own
  // call sites go through vol(), which follows the flag below.
  usingInternal = true;
}

bool SDCardManager::beginInternal() {
  // SdFat's FAT formatter refuses volumes <= 6 MB ("Card is too small"), so the
  // 3.4 MB `spiffs` partition is unusable as-is. app1 — the OTA backup slot,
  // 6.25 MB, currently all 0xFF and never booted (otadata still selects app0) —
  // is the right host for the internal store. NOTE: in-device OTA is therefore
  // not usable while internal storage is in play; flash over USB instead.
  const esp_partition_t* part =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  if (part == nullptr) {
    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  }
  if (part == nullptr || part->size <= 6u * 1024u * 1024u) {
    if (Serial) Serial.printf("[SD] no usable internal storage partition (need > 6MB)\n");
    return false;
  }
  if (!internalDev.begin(part)) {
    return false;
  }

  // Try existing FAT first (fast path on every boot after the first).
  // Two layouts are accepted: an MBR-partitioned volume (what SdFat's own
  // FatFormatter writes) and a partitionless "superfloppy" volume (what
  // mtools/mformat produces, used to preload fonts from the flasher).
  if (internalVol.begin(&internalDev)) {
    return true;
  }
  if (internalVol.begin(&internalDev, true, 0, 0)) {
    return true;
  }

  // Not formatted yet: format once, then mount.
  if (Serial) Serial.println("[SD] formatting internal storage...");
  uint8_t* work = static_cast<uint8_t*>(malloc(4096));
  if (work == nullptr) {
    return false;
  }
  FatFormatter formatter;
  const bool formatted = formatter.format(&internalDev, work);
  free(work);
  if (!formatted) {
    if (Serial) Serial.println("[SD] internal storage format failed");
    return false;
  }
  if (!internalVol.begin(&internalDev)) {
    if (Serial) Serial.println("[SD] internal storage mount failed after format");
    return false;
  }
  return true;
}

void SDCardManager::runBootSelfTest() {
  if (!Serial) return;
  constexpr const char* kMarker = "/.selftest_done";
  if (exists(kMarker)) {
    Serial.println("[SD] selftest previously passed (marker present)");
    return;
  }
  constexpr const char* kPath = "/selftest.txt";
  const String payload = "onepage-internal-storage-ok";
  const bool wrote = writeFile(kPath, payload);
  const String readBack = readFile(kPath);
  const bool match = (readBack == payload);
  const unsigned entries = static_cast<unsigned>(listFiles("/", 20).size());
  Serial.printf("[SD] SELFTEST write=%s read=%s match=%s rootEntries=%u => %s\n", wrote ? "ok" : "FAIL",
                readBack.length() ? readBack.c_str() : "(empty)", match ? "ok" : "FAIL", entries,
                (wrote && match) ? "PASS" : "FAIL");
  if (wrote && match) {
    // Persist the marker so the test runs once per format.
    writeFile(kMarker, "ok");
  }
}

namespace {
// Copy one file between two mounted FAT volumes; verify the copy by size.
bool copyOneFileVerified(FsVolume& src, FsVolume& dst, const char* path) {
  FsFile in = src.open(path, O_RDONLY);
  if (!in) return false;
  const uint32_t want = in.fileSize();
  if (dst.exists(path)) dst.remove(path);
  FsFile out = dst.open(path, O_RDWR | O_CREAT | O_TRUNC);
  if (!out) {
    in.close();
    return false;
  }
  uint8_t buf[512];
  bool ok = true;
  int n;
  while ((n = in.read(buf, sizeof(buf))) > 0) {
    if (out.write(buf, static_cast<size_t>(n)) != static_cast<size_t>(n)) {
      ok = false;
      break;
    }
  }
  out.close();
  in.close();
  if (!ok) return false;
  FsFile chk = dst.open(path, O_RDONLY);
  const bool sizeOk = chk && chk.fileSize() == want;
  if (chk) chk.close();
  return sizeOk;
}

// Recursively mirror `path` from src to dst. Copy-only: nothing on dst is
// removed except a same-named file being overwritten.
void copyTreeFiles(FsVolume& src, FsVolume& dst, const char* path, unsigned& copied, unsigned& failed) {
  FsFile dir = src.open(path);
  if (!dir || !dir.isDirectory()) return;
  char name[128];
  for (FsFile f = dir.openNextFile(); f; f = dir.openNextFile()) {
    f.getName(name, sizeof(name));
    String p = String(path);
    if (!p.endsWith("/")) p += "/";
    p += name;
    const bool isDir = f.isDirectory();
    f.close();
    if (isDir) {
      dst.mkdir(p.c_str(), true);
      copyTreeFiles(src, dst, p.c_str(), copied, failed);
    } else if (copyOneFileVerified(src, dst, p.c_str())) {
      copied++;
    } else {
      failed++;
    }
  }
  dir.close();
}
}  // namespace

void SDCardManager::migrateInternalToSd() {
  if (!nvsGetDirty()) {
    return;
  }
  // Mount the internal volume WITHOUT stealing the current working volume.
  if (!internalVol.begin(&internalDev, false) && !internalVol.begin(&internalDev, false, 0, 0)) {
    if (Serial) Serial.println("[SD] migrate: internal volume not mountable");
    return;
  }
  unsigned copied = 0, failed = 0;
  copyTreeFiles(internalVol, sd, "/", copied, failed);
  internalVol.end();
  sd.chvol();  // restore the microSD as the current working volume
  if (Serial) Serial.printf("[SD] migrate internal->SD: %u copied, %u failed\n", copied, failed);
  if (failed == 0) {
    nvsSetDirty(false);
    if (Serial) Serial.println("[SD] migration verified; internal dirty flag cleared");
  }
}

bool SDCardManager::ready() const {
  return initialized;
}

std::vector<String> SDCardManager::listFiles(const char* path, const int maxFiles) {
  std::vector<String> ret;
  if (!initialized) {
    if (Serial) Serial.printf("[%lu] [SD] not initialized, returning empty list\n", millis());
    return ret;
  }

  auto root = vol().open(path);
  if (!root) {
    if (Serial) Serial.printf("[%lu] [SD] Failed to open directory\n", millis());
    return ret;
  }
  if (!root.isDirectory()) {
    if (Serial) Serial.printf("[%lu] [SD] Path is not a directory\n", millis());
    root.close();
    return ret;
  }

  int count = 0;
  char name[128];
  for (auto f = root.openNextFile(); f && count < maxFiles; f = root.openNextFile()) {
    if (f.isDirectory()) {
      f.close();
      continue;
    }
    f.getName(name, sizeof(name));
    ret.emplace_back(name);
    f.close();
    count++;
  }
  root.close();
  return ret;
}

String SDCardManager::readFile(const char* path) {
  if (!initialized) {
    if (Serial) Serial.printf("[%lu] [SD] not initialized; cannot read file\n", millis());
    return {""};
  }

  FsFile f;
  if (!openFileForRead("SD", path, f)) {
    return {""};
  }

  String content = "";
  constexpr size_t maxSize = 50000;  // Limit to 50KB
  size_t readSize = 0;
  while (f.available() && readSize < maxSize) {
    const char c = static_cast<char>(f.read());
    content += c;
    readSize++;
  }
  f.close();
  return content;
}

bool SDCardManager::readFileToStream(const char* path, Print& out, const size_t chunkSize) {
  if (!initialized) {
    if (Serial) Serial.println("SDCardManager: not initialized; cannot read file");
    return false;
  }

  FsFile f;
  if (!openFileForRead("SD", path, f)) {
    return false;
  }

  constexpr size_t localBufSize = 256;
  uint8_t buf[localBufSize];
  const size_t toRead = (chunkSize == 0) ? localBufSize : (chunkSize < localBufSize ? chunkSize : localBufSize);

  while (f.available()) {
    const int r = f.read(buf, toRead);
    if (r > 0) {
      out.write(buf, static_cast<size_t>(r));
    } else {
      break;
    }
  }

  f.close();
  return true;
}

size_t SDCardManager::readFileToBuffer(const char* path, char* buffer, const size_t bufferSize, const size_t maxBytes) {
  if (!buffer || bufferSize == 0)
    return 0;
  if (!initialized) {
    if (Serial) Serial.println("SDCardManager: not initialized; cannot read file");
    buffer[0] = '\0';
    return 0;
  }

  FsFile f;
  if (!openFileForRead("SD", path, f)) {
    buffer[0] = '\0';
    return 0;
  }

  const size_t maxToRead = (maxBytes == 0) ? (bufferSize - 1) : min(maxBytes, bufferSize - 1);
  size_t total = 0;

  while (f.available() && total < maxToRead) {
    constexpr size_t chunk = 64;
    const size_t want = maxToRead - total;
    const size_t readLen = (want < chunk) ? want : chunk;
    const int r = f.read(buffer + total, readLen);
    if (r > 0) {
      total += static_cast<size_t>(r);
    } else {
      break;
    }
  }

  buffer[total] = '\0';
  f.close();
  return total;
}

bool SDCardManager::writeFile(const char* path, const String& content) {
  if (!initialized) {
    if (Serial) Serial.println("SDCardManager: not initialized; cannot write file");
    return false;
  }
  if (usingInternal) nvsSetDirty(true);

  // Remove existing file so we perform an overwrite rather than append
  if (vol().exists(path)) {
    vol().remove(path);
  }

  FsFile f;
  if (!openFileForWrite("SD", path, f)) {
    if (Serial) Serial.printf("Failed to open file for write: %s\n", path);
    return false;
  }

  const size_t written = f.print(content);
  f.close();
  return written == content.length();
}

bool SDCardManager::ensureDirectoryExists(const char* path) {
  if (!initialized) {
    if (Serial) Serial.println("SDCardManager: not initialized; cannot create directory");
    return false;
  }

  // Check if directory already exists
  if (vol().exists(path)) {
    FsFile dir = vol().open(path);
    if (dir && dir.isDirectory()) {
      dir.close();
      if (Serial) Serial.printf("Directory already exists: %s\n", path);
      return true;
    }
    dir.close();
  }

  // Create the directory
  if (vol().mkdir(path)) {
    if (Serial) Serial.printf("Created directory: %s\n", path);
    return true;
  } else {
    if (Serial) Serial.printf("Failed to create directory: %s\n", path);
    return false;
  }
}

bool SDCardManager::openFileForRead(const char* moduleName, const char* path, FsFile& file) {
  if (!vol().exists(path)) {
    if (Serial) Serial.printf("[%lu] [%s] File does not exist: %s\n", millis(), moduleName, path);
    return false;
  }

  file = vol().open(path, O_RDONLY);
  if (!file) {
    if (Serial) Serial.printf("[%lu] [%s] Failed to open file for reading: %s\n", millis(), moduleName, path);
    return false;
  }
  return true;
}

bool SDCardManager::openFileForRead(const char* moduleName, const std::string& path, FsFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool SDCardManager::openFileForRead(const char* moduleName, const String& path, FsFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool SDCardManager::openFileForWrite(const char* moduleName, const char* path, FsFile& file) {
  if (usingInternal) nvsSetDirty(true);
  file = vol().open(path, O_RDWR | O_CREAT | O_TRUNC);
  if (!file) {
    if (Serial) Serial.printf("[%lu] [%s] Failed to open file for writing: %s\n", millis(), moduleName, path);
    return false;
  }
  return true;
}

bool SDCardManager::openFileForWrite(const char* moduleName, const std::string& path, FsFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool SDCardManager::openFileForWrite(const char* moduleName, const String& path, FsFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool SDCardManager::removeDir(const char* path) {
  // 1. Open the directory
  auto dir = vol().open(path);
  if (!dir) {
    return false;
  }
  if (!dir.isDirectory()) {
    return false;
  }

  auto file = dir.openNextFile();
  char name[128];
  while (file) {
    String filePath = path;
    if (!filePath.endsWith("/")) {
      filePath += "/";
    }
    file.getName(name, sizeof(name));
    filePath += name;

    if (file.isDirectory()) {
      if (!removeDir(filePath.c_str())) {
        return false;
      }
    } else {
      if (!vol().remove(filePath.c_str())) {
        return false;
      }
    }
    file = dir.openNextFile();
  }

  return vol().rmdir(path);
}
