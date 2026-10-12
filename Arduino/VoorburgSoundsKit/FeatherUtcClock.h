#pragma once

#include <cstdint>

// A UTC/monotonic anchor learned from a successful LoRaWAN DeviceTimeAns.
// Reboots start without UTC. Persisting a previous date would not account for
// powered-off time. All measurement timestamps map their original window end.
class FeatherUtcClock {
public:
  static constexpr uint32_t GPS_UNIX_EPOCH_SECONDS = 315964800UL;
  static constexpr uint32_t EARLIEST_UNIX_SECONDS = 1704067200UL; // 2024-01-01

  bool synchronize(uint32_t gpsSeconds, uint32_t elapsedTicks,
                   uint32_t ticksPerSecond, uint64_t monotonicNowUs,
                   uint32_t gpsUtcOffsetSeconds) {
    if (!ticksPerSecond || gpsUtcOffsetSeconds > 255 ||
        uint64_t(elapsedTicks) > uint64_t(ticksPerSecond) * 120) return false;
    const uint64_t gpsUnix = uint64_t(gpsSeconds) + GPS_UNIX_EPOCH_SECONDS;
    if (gpsUnix < gpsUtcOffsetSeconds) return false;
    const uint64_t unixSeconds = gpsUnix - gpsUtcOffsetSeconds;
    if (unixSeconds < EARLIEST_UNIX_SECONDS || unixSeconds > UINT32_MAX) return false;
    const uint64_t utcUs = unixSeconds * 1000000ULL +
                          uint64_t(elapsedTicks) * 1000000ULL / ticksPerSecond;
    if (utcUs / 1000000ULL > UINT32_MAX) return false;
    anchorUtcUs = utcUs;
    anchorMonotonicUs = monotonicNowUs;
    synchronized = true;
    return true;
  }

  bool microsAt(uint64_t monotonicUs, uint64_t &utcUs) const {
    if (!synchronized) return false;
    uint64_t candidate;
    if (monotonicUs >= anchorMonotonicUs) {
      const uint64_t delta = monotonicUs - anchorMonotonicUs;
      if (delta > UINT64_MAX - anchorUtcUs) return false;
      candidate = anchorUtcUs + delta;
    } else {
      const uint64_t delta = anchorMonotonicUs - monotonicUs;
      if (delta > anchorUtcUs) return false;
      candidate = anchorUtcUs - delta;
    }
    if (candidate / 1000000ULL < EARLIEST_UNIX_SECONDS ||
        candidate / 1000000ULL > UINT32_MAX) return false;
    utcUs = candidate;
    return true;
  }

  bool secondsAt(uint64_t monotonicUs, uint32_t &seconds) const {
    uint64_t utcUs;
    if (!microsAt(monotonicUs, utcUs)) return false;
    seconds = static_cast<uint32_t>(utcUs / 1000000ULL);
    return true;
  }

  bool needsSynchronization(uint64_t monotonicNowUs, uint32_t periodSeconds) const {
    return !synchronized || monotonicNowUs < anchorMonotonicUs ||
           monotonicNowUs - anchorMonotonicUs >= uint64_t(periodSeconds) * 1000000ULL;
  }

private:
  bool synchronized = false;
  uint64_t anchorUtcUs = 0;
  uint64_t anchorMonotonicUs = 0;
};
