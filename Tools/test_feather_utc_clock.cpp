#include "../Arduino/VoorburgSoundsKit/FeatherUtcClock.h"
#include <cassert>
#include <cstdio>

int main() {
  FeatherUtcClock clock;
  uint32_t seconds = 123;
  uint64_t micros = 123;
  assert(!clock.secondsAt(10000000, seconds) && seconds == 123);
  assert(clock.needsSynchronization(10000000, 21600));
  const uint32_t targetUtc = 1791573000UL;
  const uint32_t gps = targetUtc - FeatherUtcClock::GPS_UNIX_EPOCH_SECONDS + 18;
  assert(clock.synchronize(gps, 93750, 62500, 10000000, 18));
  assert(clock.microsAt(10000000, micros) && micros == uint64_t(targetUtc)*1000000 + 1500000);
  assert(clock.secondsAt(9000000, seconds) && seconds == targetUtc);
  assert(clock.secondsAt(13000000, seconds) && seconds == targetUtc + 4);
  // Map the completed window, rather than the delayed publication time.
  const uint64_t windowEndUs = 310000000;
  assert(clock.secondsAt(windowEndUs, seconds) && seconds == targetUtc + 301);
  const uint32_t fixedWindowEnd = seconds;
  assert(clock.secondsAt(windowEndUs + 70000000, seconds) && seconds == fixedWindowEnd + 70);
  assert(!clock.needsSynchronization(10000000 + 21599ULL*1000000, 21600));
  assert(clock.needsSynchronization(10000000 + 21600ULL*1000000, 21600));
  assert(!clock.synchronize(gps, 0, 0, 0, 18));
  assert(!clock.synchronize(gps, 7500001, 62500, 0, 18));
  assert(!clock.synchronize(0, 0, 62500, 0, 18));
  assert(!clock.synchronize(UINT32_MAX, 0, 62500, 0, 18));
  assert(clock.secondsAt(10000000, seconds) && seconds == targetUtc + 1);
  const uint32_t laterUtc = 0x80000001UL;
  assert(clock.synchronize(laterUtc - FeatherUtcClock::GPS_UNIX_EPOCH_SECONDS + 18,
                           0, 62500, 9000000000ULL, 18));
  assert(clock.secondsAt(9000000000ULL, seconds) && seconds == laterUtc);
  assert(!clock.microsAt(UINT64_MAX, micros));
  FeatherUtcClock reboot;
  assert(!reboot.secondsAt(10000000, seconds));
  std::puts("UTC clock: GPS leap correction, fractional anchor, fixed window end, unsigned time, invalid replies and reboot fallback passed");
}
