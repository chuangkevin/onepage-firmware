#pragma once

// FlashBlockDevice — SdFat block device on an ESP-IDF flash partition.
//
// Purpose: let the reader run with NO microSD card by mounting a FAT volume
// on a spare internal-flash partition. The whole upper stack (HalStorage /
// HalFile / every consumer) is unchanged: it still talks SdFat, just to a
// different volume.
//
// ESP flash needs erase-before-write at 4 KB granularity, so writes go through
// a read-modify-write cache of one erase block. This is NOT wear-levelled —
// acceptable for a reader that writes infrequently (settings, reading
// position); the high-frequency state writes should be coalesced by callers.

#include <SdFat.h>  // brings in FsBlockDevice / FsBlockDeviceInterface, FatFormatter, Sector_t

class FlashBlockDevice : public FsBlockDeviceInterface {
 public:
  static constexpr uint32_t kSectorSize = 512;
  static constexpr uint32_t kEraseBlock = 4096;  // ESP flash sector size
  static constexpr uint32_t kSectorsPerBlock = kEraseBlock / kSectorSize;

  // Bind to a partition. Does not read/write yet.
  bool begin(const esp_partition_t* part);

  void end() override;

  bool isBusy() override { return false; }

  bool readSector(Sector_t sector, uint8_t* dst) override;
  bool readSectors(Sector_t sector, uint8_t* dst, size_t ns) override;

  Sector_t sectorCount() override;

  // Flush the pending erase block. SdFat calls this before unmount and after
  // multi-sector writes.
  bool syncDevice() override { return flushBlock(); }

  bool writeSector(Sector_t sector, const uint8_t* src) override;
  bool writeSectors(Sector_t sector, const uint8_t* src, size_t ns) override;

 private:
  bool loadBlock(uint32_t block);
  bool flushBlock();

  const esp_partition_t* part_ = nullptr;
  uint8_t cache_[kEraseBlock];  // 4 KB read-modify-write buffer
  uint32_t cachedBlock_ = 0xFFFFFFFF;
  bool dirty_ = false;
};
