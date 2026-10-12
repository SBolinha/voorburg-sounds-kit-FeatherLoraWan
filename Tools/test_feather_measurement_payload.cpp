#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>

#include "FeatherMeasurementPayload.h"

static void exactPicoFixture() {
  FeatherMeasurementPayload payload;
  // Matches the documented Pico formatter fixture 8F4EA51E92009FC86A.
  const uint8_t expected[9] = {0x8F, 0x4E, 0xA5, 0x1E, 0x92,
                               0x00, 0x9F, 0xC8, 0x6A};
  assert(payload.encode(71.5, 39.0, 82.5, 3.75, 73.0, 0x6AC89F00ULL));
  assert(sizeof(payload.bytes) == FeatherMeasurementPayload::MAX_SIZE);
  assert(payload.length == FeatherMeasurementPayload::SIZE);
  assert(std::memcmp(payload.bytes, expected, sizeof(expected)) == 0);
  assert(payload.windowEndUtc() == 0x6AC89F00UL);
}

static void unsignedTimestampBoundaries() {
  FeatherMeasurementPayload payload;
  assert(payload.encode(1, 2, 3, 4, 5, 0x80000001ULL));
  const uint8_t post2038[4] = {0x01, 0x00, 0x00, 0x80};
  assert(std::memcmp(payload.bytes + 5, post2038, 4) == 0);
  assert(payload.windowEndUtc() == 0x80000001UL);
  assert(payload.encode(1, 2, 3, 4, 5, UINT32_MAX));
  const uint8_t maximum[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  assert(std::memcmp(payload.bytes + 5, maximum, 4) == 0);
  assert(payload.windowEndUtc() == UINT32_MAX);
}

static void noUtcUsesLegacyAndTransitions() {
  FeatherMeasurementPayload payload;
  const uint8_t expected[5] = {0x8F, 0x4E, 0xA5, 0x1E, 0x92};
  const uint8_t emptyTimestamp[4] = {};
  assert(payload.encode(71.5, 39.0, 82.5, 3.75, 73.0, 0, false));
  assert(payload.length == FeatherMeasurementPayload::LEGACY_SIZE);
  assert(std::memcmp(payload.bytes, expected, sizeof(expected)) == 0);
  assert(payload.windowEndUtc() == 0);
  assert(payload.encode(71.5, 39.0, 82.5, 3.75, 73.0, 0x6AC89F00ULL, true));
  assert(payload.length == FeatherMeasurementPayload::SIZE);
  assert(payload.windowEndUtc() == 0x6AC89F00UL);
  // A stale timestamp is ignored and cleared once the clock is unavailable.
  assert(payload.encode(71.5, 39.0, 82.5, 3.75, 73.0, UINT64_MAX, false));
  assert(payload.length == FeatherMeasurementPayload::LEGACY_SIZE);
  assert(std::memcmp(payload.bytes, expected, sizeof(expected)) == 0);
  assert(std::memcmp(payload.bytes + 5, emptyTimestamp, 4) == 0);
  assert(payload.windowEndUtc() == 0);
}

static void invalidKnownClockPreservesPayload() {
  FeatherMeasurementPayload payload;
  const uint8_t empty[FeatherMeasurementPayload::MAX_SIZE] = {};
  assert(!payload.encode(1, 2, 3, 4, 5, 0));
  assert(std::memcmp(payload.bytes, empty, sizeof(empty)) == 0);
  assert(payload.length == FeatherMeasurementPayload::LEGACY_SIZE);
  assert(!payload.encode(1, 2, 3, 4, 5, UINT64_C(1) << 32));
  assert(!payload.encode(1, 2, 3, 4, 5, UINT64_MAX));
  assert(payload.encode(1, 2, 3, 4, 5, 0x6AC89F00ULL));
  uint8_t previous[FeatherMeasurementPayload::MAX_SIZE];
  std::memcpy(previous, payload.bytes, sizeof(previous));
  assert(!payload.encode(10, 20, 30, 40, 50, 0));
  assert(!payload.encode(10, 20, 30, 40, 50, UINT64_C(1) << 32));
  assert(std::memcmp(previous, payload.bytes, sizeof(previous)) == 0);
  assert(payload.length == FeatherMeasurementPayload::SIZE);
  assert(payload.encode(1, 2, 3, 4, 5, 0, false));
  std::memcpy(previous, payload.bytes, sizeof(previous));
  assert(!payload.encode(10, 20, 30, 40, 50, UINT64_MAX));
  assert(std::memcmp(previous, payload.bytes, sizeof(previous)) == 0);
  assert(payload.length == FeatherMeasurementPayload::LEGACY_SIZE);
}

static void clampAndTruncateLikePico() {
  FeatherMeasurementPayload payload;
  assert(payload.encode(-4, 127.5, 300, 32, 35.99, 0x6AC89F00ULL));
  const uint8_t expected[5] = {0, 255, 255, 255, 71};
  assert(std::memcmp(payload.bytes, expected, sizeof(expected)) == 0);
  assert(payload.encode(-4, 127.5, 300, 32, 35.99, 0, false));
  assert(payload.length == FeatherMeasurementPayload::LEGACY_SIZE);
  assert(std::memcmp(payload.bytes, expected, sizeof(expected)) == 0);
  assert(FeatherMeasurementPayload::toUQ7_1(0.49) == 0);
  assert(FeatherMeasurementPayload::toUQ7_1(0.5) == 1);
  assert(FeatherMeasurementPayload::toUQ7_1(127.49) == 254);
  assert(FeatherMeasurementPayload::toUQ5_3(-0.5) == 0);
  assert(FeatherMeasurementPayload::toUQ5_3(0.124) == 0);
  assert(FeatherMeasurementPayload::toUQ5_3(0.125) == 1);
  assert(FeatherMeasurementPayload::toUQ5_3(31.874) == 254);
  assert(FeatherMeasurementPayload::toUQ5_3(31.875) == 255);
}

static void nonfiniteValuesAreDefined() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  FeatherMeasurementPayload payload;
  assert(payload.encode(nan, inf, -inf, nan, inf, 0x6AC89F00ULL));
  const uint8_t expected[5] = {0, 255, 0, 0, 255};
  assert(std::memcmp(payload.bytes, expected, sizeof(expected)) == 0);
  assert(FeatherMeasurementPayload::toUQ5_3(inf) == 255);
  assert(FeatherMeasurementPayload::toUQ5_3(-inf) == 0);
  assert(FeatherMeasurementPayload::toUQ7_1(-0.0) == 0);
}

static void batteryLayoutsAndValidation() {
  FeatherMeasurementPayload payload;
  assert(payload.encode(35, 29.5, 76, 4.5, 44.5, 0, false));
  assert(payload.appendBattery(2280));
  const uint8_t withoutUtc[7] = {0x46, 0x3B, 0x98, 0x24, 0x59, 0xE8, 0x08};
  assert(payload.length == 7);
  assert(std::memcmp(payload.bytes, withoutUtc, sizeof(withoutUtc)) == 0);
  assert(payload.windowEndUtc() == 0);
  assert(!payload.appendBattery(1));
  assert(payload.encode(35, 29.5, 76, 4.5, 44.5, 1791628756ULL));
  assert(payload.appendBattery(4095));
  assert(payload.length == 11 && payload.bytes[9] == 255 && payload.bytes[10] == 15);
  assert(payload.windowEndUtc() == 1791628756UL);
  assert(!payload.appendBattery(0));
  assert(payload.encode(35, 29.5, 76, 4.5, 44.5, 0, false));
  for (unsigned i = 5; i < sizeof(payload.bytes); ++i) assert(payload.bytes[i] == 0);
  assert(!payload.appendBattery(4096));
  assert(payload.length == 5);
  assert(payload.appendBattery(0));
  assert(payload.length == 7 && payload.bytes[5] == 0 && payload.bytes[6] == 0);
}

int main() {
  exactPicoFixture();
  unsignedTimestampBoundaries();
  noUtcUsesLegacyAndTransitions();
  invalidKnownClockPreservesPayload();
  clampAndTruncateLikePico();
  nonfiniteValuesAreDefined();
  batteryLayoutsAndValidation();
  std::cout << "Feather payload: Pico fixture, unsigned UTC, legacy fallback and transitions, invalid known clock, quantization, nonfinite values, and 5/7/9/11-byte battery layouts passed\n";
}
