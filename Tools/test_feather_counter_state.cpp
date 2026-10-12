#include <cassert>
#include <cstdint>
#include <iostream>
#include "FeatherCounterState.h"

typedef FeatherCounterState State;

struct Storage {
  State::Record record = {};
  bool exists = false;
  bool fail = false;
  uint32_t attempts = 0;
  uint32_t writes = 0;
};

static bool durableWrite(const State::Record &record, void *context) {
  Storage &storage = *static_cast<Storage *>(context);
  ++storage.attempts;
  if (storage.fail) return false;
  assert(State::validRecord(record));
  storage.record = record;
  storage.exists = true;
  ++storage.writes;
  return true;
}

static void rebootAndReservationBoundary() {
  Storage storage;
  State live;
  const uint64_t identity = 0x0123456789ABCDEFULL;
  assert(live.begin(identity, 0, false, storage.record, durableWrite, &storage));
  assert(live.startUp() == 0 && live.blockEnd() == 256);
  assert(storage.writes == 1);
  // Simulate an LMIC retry, then all counters in the first reservation.
  assert(live.ensureReserved(0));
  assert(live.ensureReserved(0));
  for (uint32_t counter = 1; counter < 256; ++counter)
    assert(live.ensureReserved(counter));
  assert(storage.writes == 1);
  assert(live.ensureReserved(256));
  assert(live.blockEnd() == 512 && storage.writes == 2);
  assert(live.saveDown(7));

  State rebooted;
  assert(rebooted.begin(identity, 0, true, storage.record, durableWrite, &storage));
  assert(rebooted.startUp() == 512 && rebooted.startDown() == 7);
  assert(!rebooted.ensureReserved(511));
  assert(rebooted.ensureReserved(512));
  assert(rebooted.blockEnd() == 768);
  assert(storage.record.nextDown == 7);
}

static void failedWritesDoNotAuthorizeCounters() {
  Storage storage;
  storage.fail = true;
  State state;
  assert(!state.begin(11, 0, false, storage.record, durableWrite, &storage));
  assert(!state.ensureReserved(0));
  assert(!state.saveDown(1));
  assert(!storage.exists);

  storage.fail = false;
  assert(state.begin(11, 0, false, storage.record, durableWrite, &storage));
  storage.fail = true;
  assert(!state.ensureReserved(256));
  assert(state.blockEnd() == 256 && storage.record.reservedEnd == 256);
  assert(state.ensureReserved(255));
  assert(!state.saveDown(3));
  assert(state.startDown() == 0 && storage.record.nextDown == 0);

  State rebooted;
  assert(rebooted.begin(11, 0, true, storage.record, durableWrite, &storage));
  assert(rebooted.startUp() == 256);
  assert(!rebooted.ensureReserved(256));
  assert(rebooted.blockEnd() == 256);
  storage.fail = false;
  assert(rebooted.ensureReserved(256));
  assert(rebooted.saveDown(3));
  assert(rebooted.startDown() == 3 && storage.record.nextDown == 3);
}

static void corruptedOrWrongIdentityCannotResetState() {
  Storage storage;
  State state;
  assert(state.begin(22, 17, false, storage.record, durableWrite, &storage));
  assert(state.startUp() == 17 && state.blockEnd() == 273);
  const uint32_t writes = storage.writes;
  State rejected;
  State::Record corrupt = storage.record;
  corrupt.reservedEnd ^= 1;
  assert(!rejected.begin(22, 0, true, corrupt, durableWrite, &storage));
  assert(!rejected.ensureReserved(0));
  assert(!rejected.begin(23, 0, true, storage.record, durableWrite, &storage));
  State::Record wrongVersion = storage.record;
  ++wrongVersion.version;
  wrongVersion.crc = State::recordChecksum(wrongVersion);
  assert(!rejected.begin(22, 0, true, wrongVersion, durableWrite, &storage));
  State::Record wrongMagic = storage.record;
  wrongMagic.magic = 0;
  wrongMagic.crc = State::recordChecksum(wrongMagic);
  assert(!rejected.begin(22, 0, true, wrongMagic, durableWrite, &storage));
  assert(storage.writes == writes);
}

static void downlinkReplayAndReboot() {
  Storage storage;
  State state;
  assert(state.begin(33, 0, false, storage.record, durableWrite, &storage));
  assert(state.saveDown(10));
  const uint32_t writes = storage.writes;
  assert(state.saveDown(10));
  assert(!state.saveDown(9));
  assert(state.startDown() == 10 && storage.writes == writes);
  State rebooted;
  assert(rebooted.begin(33, 0, true, storage.record, durableWrite, &storage));
  assert(rebooted.startDown() == 10);
  assert(!rebooted.saveDown(1));
  assert(rebooted.ensureReserved(rebooted.startUp()));
  assert(storage.record.nextDown == 10);
}

static void counterOverflowIsTerminal() {
  Storage storage;
  State state;
  assert(state.begin(44, UINT32_MAX - 256, false, storage.record,
                     durableWrite, &storage));
  assert(state.blockEnd() == UINT32_MAX);
  assert(state.ensureReserved(UINT32_MAX - 1));
  assert(!state.ensureReserved(UINT32_MAX));
  const uint32_t writes = storage.writes;
  State rebooted;
  assert(rebooted.begin(44, 0, true, storage.record, durableWrite, &storage));
  assert(!rebooted.ensureReserved(rebooted.startUp()));
  assert(storage.writes == writes);
  State impossible;
  assert(!impossible.begin(44, UINT32_MAX - 255, false, storage.record,
                           durableWrite, &storage));
  assert(storage.writes == writes);
}

static void serializedRecordsAreStableAndChecked() {
  Storage storage;
  State state;
  const uint64_t identity = 0x0123456789ABCDEFULL;
  assert(state.begin(identity, 0, false, storage.record, durableWrite, &storage));
  assert(state.saveDown(0x11223344));
  uint8_t bytes[State::ENCODED_SIZE];
  State::encodeRecord(storage.record, bytes);
  assert(bytes[8] == 0xEF && bytes[15] == 0x01);
  assert(bytes[16] == 0x00 && bytes[17] == 0x01);
  assert(bytes[20] == 0x44 && bytes[23] == 0x11);
  State::Record decoded = {};
  assert(State::decodeRecord(bytes, sizeof(bytes), decoded));
  assert(State::validRecord(decoded, identity));
  assert(decoded.reservedEnd == 256 && decoded.nextDown == 0x11223344);
  // Every persisted byte, including the checksum, must detect a bit flip.
  for (uint32_t i = 0; i < sizeof(bytes); ++i) {
    bytes[i] ^= 1;
    assert(!State::decodeRecord(bytes, sizeof(bytes), decoded));
    bytes[i] ^= 1;
  }
  assert(!State::decodeRecord(bytes, sizeof(bytes) - 1, decoded));
  assert(!State::decodeRecord(nullptr, sizeof(bytes), decoded));
}

int main() {
  rebootAndReservationBoundary();
  failedWritesDoNotAuthorizeCounters();
  corruptedOrWrongIdentityCannotResetState();
  downlinkReplayAndReboot();
  counterOverflowIsTerminal();
  serializedRecordsAreStableAndChecked();
  std::cout << "Feather counter persistence tests passed\n";
}
