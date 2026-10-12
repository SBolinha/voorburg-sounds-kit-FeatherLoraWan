#ifndef FEATHER_COUNTER_STATE_H
#define FEATHER_COUNTER_STATE_H

#include <cstdint>

// Reserve uplink counters durably before LMIC can transmit them. A reboot
// starts beyond the entire last reservation, including its unused tail.
class FeatherCounterState {
public:
  struct Record {
    uint32_t magic;
    uint32_t version;
    uint64_t identityTag;
    uint32_t reservedEnd;
    uint32_t nextDown;
    uint32_t crc;
  };

  typedef bool (*DurableWrite)(const Record &, void *);
  static const uint32_t ENCODED_SIZE = 28;
  static const uint32_t COUNTERS_PER_BLOCK = 256;

  bool begin(uint64_t identityTag, uint32_t initialUp, bool haveRecord,
             const Record &record, DurableWrite durableWrite,
             void *context = nullptr) {
    ready_ = false;
    startUp_ = 0;
    nextDown_ = 0;
    reservedEnd_ = 0;
    durableWrite_ = durableWrite;
    context_ = context;
    if (!durableWrite_) return false;

    if (haveRecord) {
      // An existing corrupt record or another identity requires explicit
      // reprovisioning; silently resetting here could reuse counters on air.
      if (!validRecord(record, identityTag)) return false;
      identityTag_ = identityTag;
      startUp_ = record.reservedEnd;
      nextDown_ = record.nextDown;
      reservedEnd_ = record.reservedEnd;
      ready_ = true;
      return true;
    }

    if (initialUp > UINT32_MAX - COUNTERS_PER_BLOCK) return false;
    const Record first = makeRecord(identityTag,
                                    initialUp + COUNTERS_PER_BLOCK, 0);
    if (!durableWrite_(first, context_)) return false;
    identityTag_ = identityTag;
    startUp_ = initialUp;
    nextDown_ = 0;
    reservedEnd_ = first.reservedEnd;
    ready_ = true;
    return true;
  }

  // Call before every possible transmission, including MAC-only packets.
  // Rechecking a counter during a retry does not consume another reservation.
  bool ensureReserved(uint32_t nextUp) {
    if (!ready_ || nextUp < startUp_ || nextUp == UINT32_MAX) return false;
    if (nextUp < reservedEnd_) return true;

    const uint64_t blocks =
        (static_cast<uint64_t>(nextUp) - reservedEnd_) / COUNTERS_PER_BLOCK + 1;
    const uint64_t end = static_cast<uint64_t>(reservedEnd_) +
                         blocks * COUNTERS_PER_BLOCK;
    if (end > UINT32_MAX) return false;
    const Record updated = makeRecord(identityTag_, static_cast<uint32_t>(end),
                                      nextDown_);
    if (!durableWrite_(updated, context_)) return false;
    reservedEnd_ = updated.reservedEnd;
    return true;
  }

  // Store LMIC's next expected downlink counter after an authenticated frame.
  bool saveDown(uint32_t nextDown) {
    if (!ready_ || nextDown < nextDown_) return false;
    if (nextDown == nextDown_) return true;
    const Record updated = makeRecord(identityTag_, reservedEnd_, nextDown);
    if (!durableWrite_(updated, context_)) return false;
    nextDown_ = nextDown;
    return true;
  }

  uint32_t startUp() const { return startUp_; }
  uint32_t startDown() const { return nextDown_; }
  uint32_t blockEnd() const { return reservedEnd_; }

  // The on-disk format is explicitly little-endian and excludes all C++
  // struct padding. CRC covers the first 24 serialized bytes.
  static void encodeRecord(const Record &record, uint8_t *bytes) {
    put32(bytes, record.magic);
    put32(bytes + 4, record.version);
    put64(bytes + 8, record.identityTag);
    put32(bytes + 16, record.reservedEnd);
    put32(bytes + 20, record.nextDown);
    put32(bytes + 24, record.crc);
  }

  static bool decodeRecord(const uint8_t *bytes, uint32_t length,
                           Record &record) {
    if (!bytes || length != ENCODED_SIZE) return false;
    Record decoded = {};
    decoded.magic = get32(bytes);
    decoded.version = get32(bytes + 4);
    decoded.identityTag = get64(bytes + 8);
    decoded.reservedEnd = get32(bytes + 16);
    decoded.nextDown = get32(bytes + 20);
    decoded.crc = get32(bytes + 24);
    if (!validRecord(decoded)) return false;
    record = decoded;
    return true;
  }

  static uint32_t recordChecksum(const Record &record) {
    uint8_t bytes[ENCODED_SIZE];
    encodeRecord(record, bytes);
    uint32_t crc = UINT32_MAX;
    for (uint32_t i = 0; i < ENCODED_SIZE - 4; ++i) {
      crc ^= bytes[i];
      for (uint32_t bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320UL : 0);
    }
    return ~crc;
  }

  static bool validRecord(const Record &record) {
    return record.magic == MAGIC && record.version == VERSION &&
           record.crc == recordChecksum(record);
  }

  static bool validRecord(const Record &record, uint64_t identityTag) {
    return validRecord(record) && record.identityTag == identityTag;
  }

private:
  static const uint32_t MAGIC = 0x46435452UL; // "FCTR"
  static const uint32_t VERSION = 1;
  bool ready_ = false;
  uint64_t identityTag_ = 0;
  uint32_t startUp_ = 0;
  uint32_t nextDown_ = 0;
  uint32_t reservedEnd_ = 0;
  DurableWrite durableWrite_ = nullptr;
  void *context_ = nullptr;

  static Record makeRecord(uint64_t identityTag, uint32_t reservedEnd,
                           uint32_t nextDown) {
    Record record = {};
    record.magic = MAGIC;
    record.version = VERSION;
    record.identityTag = identityTag;
    record.reservedEnd = reservedEnd;
    record.nextDown = nextDown;
    record.crc = recordChecksum(record);
    return record;
  }

  static void put32(uint8_t *bytes, uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i)
      bytes[i] = static_cast<uint8_t>(value >> (i * 8));
  }

  static void put64(uint8_t *bytes, uint64_t value) {
    for (uint32_t i = 0; i < 8; ++i)
      bytes[i] = static_cast<uint8_t>(value >> (i * 8));
  }

  static uint32_t get32(const uint8_t *bytes) {
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4; ++i)
      value |= static_cast<uint32_t>(bytes[i]) << (i * 8);
    return value;
  }

  static uint64_t get64(const uint8_t *bytes) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < 8; ++i)
      value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
    return value;
  }
};

#endif
