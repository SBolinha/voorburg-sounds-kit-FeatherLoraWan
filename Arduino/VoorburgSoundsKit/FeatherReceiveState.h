#ifndef FEATHER_RECEIVE_STATE_H
#define FEATHER_RECEIVE_STATE_H

#include <cstdint>

// Keep authenticated receive-window settings across ABP reboots. This record
// is independent of frame counters; loading it never changes counter state.
class FeatherReceiveState {
public:
  struct Record {
    uint32_t magic;
    uint32_t version;
    uint64_t identityTag;
    uint8_t rx1Delay;
    uint8_t rx1DrOffset;
    uint8_t rx2DataRateIndex;
    uint8_t rx2Answer;
    uint8_t rxTimingAnswer;
    uint8_t reserved;
    uint16_t reserved2;
    uint32_t rx2Frequency;
    uint32_t crc;
  };

  static const uint32_t ENCODED_SIZE = 32;

  // Only an absent record should use these defaults. Existing invalid records
  // must be rejected, including records made for a different device identity.
  // Inputs are wide so invalid values cannot silently wrap into valid bytes.
  // Invalid input produces an invalid record; callers must use validRecord().
  static Record makeRecord(uint64_t identityTag, uint32_t rx1Delay = 1,
                           uint32_t rx1DrOffset = 0,
                           uint32_t rx2DataRateIndex = 0,
                           uint32_t rx2Frequency = 869525000,
                           uint32_t rx2Answer = 0,
                           uint32_t rxTimingAnswer = 0) {
    Record record = {};
    if (!validParameters(rx1Delay, rx1DrOffset, rx2DataRateIndex,
                         rx2Frequency, rx2Answer, rxTimingAnswer)) return record;
    record.magic = MAGIC;
    record.version = VERSION;
    record.identityTag = identityTag;
    record.rx1Delay = static_cast<uint8_t>(rx1Delay);
    record.rx1DrOffset = static_cast<uint8_t>(rx1DrOffset);
    record.rx2DataRateIndex = static_cast<uint8_t>(rx2DataRateIndex);
    record.rx2Answer = static_cast<uint8_t>(rx2Answer);
    record.rxTimingAnswer = static_cast<uint8_t>(rxTimingAnswer);
    record.rx2Frequency = rx2Frequency;
    record.crc = recordChecksum(record);
    return record;
  }

  // Explicit little-endian encoding excludes all struct padding. The byte
  // bytes at offsets 21..23 are reserved and must remain zero. Persist pending
  // MAC answers as well as the settings so a reboot can repeat negotiation.
  static void encodeRecord(const Record &record, uint8_t *bytes) {
    put32(bytes, record.magic);
    put32(bytes + 4, record.version);
    put64(bytes + 8, record.identityTag);
    bytes[16] = record.rx1Delay;
    bytes[17] = record.rx1DrOffset;
    bytes[18] = record.rx2DataRateIndex;
    bytes[19] = record.rx2Answer;
    bytes[20] = record.rxTimingAnswer;
    bytes[21] = record.reserved;
    bytes[22] = static_cast<uint8_t>(record.reserved2);
    bytes[23] = static_cast<uint8_t>(record.reserved2 >> 8);
    put32(bytes + 24, record.rx2Frequency);
    put32(bytes + 28, record.crc);
  }

  static bool decodeRecord(const uint8_t *bytes, uint32_t length,
                           Record &record) {
    if (!bytes || length != ENCODED_SIZE) return false;
    Record decoded = {};
    decoded.magic = get32(bytes);
    decoded.version = get32(bytes + 4);
    decoded.identityTag = get64(bytes + 8);
    decoded.rx1Delay = bytes[16];
    decoded.rx1DrOffset = bytes[17];
    decoded.rx2DataRateIndex = bytes[18];
    decoded.rx2Answer = bytes[19];
    decoded.rxTimingAnswer = bytes[20];
    decoded.reserved = bytes[21];
    decoded.reserved2 = static_cast<uint16_t>(bytes[22]) |
                        (static_cast<uint16_t>(bytes[23]) << 8);
    decoded.rx2Frequency = get32(bytes + 24);
    decoded.crc = get32(bytes + 28);
    if (!validRecord(decoded)) return false;
    record = decoded;
    return true;
  }

  // CRC-32/ISO-HDLC covers the first 28 serialized bytes, not the CRC itself.
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
           record.reserved == 0 && record.reserved2 == 0 &&
           validParameters(record.rx1Delay, record.rx1DrOffset,
                           record.rx2DataRateIndex, record.rx2Frequency,
                           record.rx2Answer, record.rxTimingAnswer) &&
           record.crc == recordChecksum(record);
  }

  static bool validRecord(const Record &record, uint64_t identityTag) {
    return validRecord(record) && record.identityTag == identityTag;
  }

private:
  static const uint32_t MAGIC = 0x46525853UL; // "FRXS"
  static const uint32_t VERSION = 1;

  static bool validParameters(uint32_t delay, uint32_t offset,
                              uint32_t dataRate, uint32_t frequency,
                              uint32_t rx2Answer, uint32_t rxTimingAnswer) {
    const bool validRx2Answer = rx2Answer == 0 ||
        (rx2Answer >= 0x80 && rx2Answer <= 0x87) ||
        (rx2Answer >= 0xC0 && rx2Answer <= 0xC7);
    return delay >= 1 && delay <= 15 && offset <= 3 && dataRate <= 6 &&
           frequency >= 863000000 && frequency <= 870000000 &&
           validRx2Answer && rxTimingAnswer <= 2;
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
