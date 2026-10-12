#ifndef FEATHER_MEASUREMENT_PAYLOAD_H
#define FEATHER_MEASUREMENT_PAYLOAD_H

#include <cmath>
#include <cstddef>
#include <cstdint>

// FPort 1: five measurement values, with a four-byte measurement window-end
// timestamp when real UTC is available. An optional raw battery ADC reading
// adds two bytes at the end. Multi-byte values are little-endian.
struct FeatherMeasurementPayload {
  static constexpr std::size_t LEGACY_SIZE = 5;
  static constexpr std::size_t SIZE = 9;
  static constexpr std::size_t MAX_SIZE = 11;
  uint8_t bytes[MAX_SIZE] = {};
  uint8_t length = LEGACY_SIZE;

  // No UTC produces a legacy five-byte payload. If the caller claims valid
  // UTC, a zero or out-of-range timestamp fails without changing bytes/length.
  // Uptime is never a valid substitute for real UTC.
  bool encode(double mean, double minimum, double maximum, double stddev,
              double leq, uint64_t windowEndUtcSeconds,
              bool clockValid = true) {
    if (clockValid && (windowEndUtcSeconds == 0 ||
        windowEndUtcSeconds > UINT32_MAX)) return false;

    const uint32_t timestamp = clockValid ?
      static_cast<uint32_t>(windowEndUtcSeconds) : 0;
    const uint8_t encoded[SIZE] = {
      toUQ7_1(mean), toUQ7_1(minimum), toUQ7_1(maximum), toUQ5_3(stddev),
      toUQ7_1(leq), static_cast<uint8_t>(timestamp),
      static_cast<uint8_t>(timestamp >> 8),
      static_cast<uint8_t>(timestamp >> 16),
      static_cast<uint8_t>(timestamp >> 24)
    };
    for (std::size_t i = 0; i < SIZE; ++i) bytes[i] = encoded[i];
    bytes[9] = bytes[10] = 0;
    length = clockValid ? SIZE : LEGACY_SIZE;
    return true;
  }

  uint32_t windowEndUtc() const {
    if (length != SIZE && length != MAX_SIZE) return 0;
    return static_cast<uint32_t>(bytes[5]) |
      (static_cast<uint32_t>(bytes[6]) << 8) |
      (static_cast<uint32_t>(bytes[7]) << 16) |
      (static_cast<uint32_t>(bytes[8]) << 24);
  }

  bool appendBattery(uint16_t adc) {
    if (adc > 4095 || (length != LEGACY_SIZE && length != SIZE)) return false;
    bytes[length++] = static_cast<uint8_t>(adc);
    bytes[length++] = static_cast<uint8_t>(adc >> 8);
    return true;
  }

  static uint8_t toUQ7_1(double value) { return quantize(value, 127.5, 2.0); }
  static uint8_t toUQ5_3(double value) { return quantize(value, 31.875, 8.0); }

private:
  static uint8_t quantize(double value, double maximum, double scale) {
    // Preserve the Pico's saturation and truncation rules. Handle NaN before
    // casting; infinities saturate to their corresponding endpoint.
    if (std::isnan(value) || value <= 0.0) return 0;
    if (value >= maximum) return 255;
    if (!std::isfinite(value)) return 0;
    return static_cast<uint8_t>(value * scale);
  }
};

#endif
