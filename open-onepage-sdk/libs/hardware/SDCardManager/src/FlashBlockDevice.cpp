#include "FlashBlockDevice.h"

#include <cstring>
#include <esp_partition.h>

bool FlashBlockDevice::begin(const esp_partition_t* part) {
  if (part == nullptr || part->size < kEraseBlock) {
    return false;
  }
  part_ = part;
  cachedBlock_ = 0xFFFFFFFF;
  dirty_ = false;
  return true;
}

void FlashBlockDevice::end() {
  flushBlock();
  part_ = nullptr;
  cachedBlock_ = 0xFFFFFFFF;
  dirty_ = false;
}

Sector_t FlashBlockDevice::sectorCount() {
  return part_ ? static_cast<Sector_t>(part_->size / kSectorSize) : 0;
}

bool FlashBlockDevice::readSector(Sector_t sector, uint8_t* dst) {
  return readSectors(sector, dst, 1);
}

bool FlashBlockDevice::readSectors(Sector_t sector, uint8_t* dst, size_t ns) {
  if (part_ == nullptr || dst == nullptr || ns == 0) {
    return false;
  }
  const uint64_t off = static_cast<uint64_t>(sector) * kSectorSize;
  const uint64_t len = static_cast<uint64_t>(ns) * kSectorSize;
  if (off + len > part_->size) {
    return false;
  }
  return esp_partition_read(part_, off, dst, len) == ESP_OK;
}

bool FlashBlockDevice::loadBlock(uint32_t block) {
  if (cachedBlock_ == block) {
    return true;
  }
  if (!flushBlock()) {
    return false;
  }
  const uint64_t off = static_cast<uint64_t>(block) * kEraseBlock;
  if (off + kEraseBlock > part_->size) {
    return false;
  }
  if (esp_partition_read(part_, off, cache_, kEraseBlock) != ESP_OK) {
    return false;
  }
  cachedBlock_ = block;
  dirty_ = false;
  return true;
}

bool FlashBlockDevice::flushBlock() {
  if (!dirty_) {
    return true;
  }
  if (part_ == nullptr) {
    return false;
  }
  const uint64_t off = static_cast<uint64_t>(cachedBlock_) * kEraseBlock;
  if (esp_partition_erase_range(part_, off, kEraseBlock) != ESP_OK) {
    return false;
  }
  if (esp_partition_write(part_, off, cache_, kEraseBlock) != ESP_OK) {
    return false;
  }
  dirty_ = false;
  return true;
}

bool FlashBlockDevice::writeSector(Sector_t sector, const uint8_t* src) {
  return writeSectors(sector, src, 1);
}

bool FlashBlockDevice::writeSectors(Sector_t sector, const uint8_t* src, size_t ns) {
  if (part_ == nullptr || src == nullptr || ns == 0) {
    return false;
  }
  for (size_t i = 0; i < ns; ++i) {
    const Sector_t s = sector + i;
    const uint32_t block = s / kSectorsPerBlock;
    const uint32_t within = s % kSectorsPerBlock;
    if (!loadBlock(block)) {
      return false;
    }
    std::memcpy(cache_ + within * kSectorSize, src + i * kSectorSize, kSectorSize);
    dirty_ = true;
  }
  return true;
}
