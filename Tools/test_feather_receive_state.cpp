#include "FeatherReceiveState.h"

#include <cassert>
#include <cstdio>
#include <cstring>

typedef FeatherReceiveState State;

static void expectRejected(State::Record record) {
  // Recompute a valid CRC: field validation must reject semantic corruption
  // even when the serialized record is otherwise internally consistent.
  record.crc = State::recordChecksum(record);
  assert(!State::validRecord(record));
  uint8_t bytes[State::ENCODED_SIZE];
  State::encodeRecord(record, bytes);
  State::Record output = State::makeRecord(99);
  const uint64_t originalIdentity = output.identityTag;
  assert(!State::decodeRecord(bytes, sizeof(bytes), output));
  assert(output.identityTag == originalIdentity);
  assert(State::validRecord(output, originalIdentity));
}

int main() {
  const uint64_t identity = UINT64_C(0x0123456789abcdef);
  const State::Record defaults = State::makeRecord(identity);
  assert(State::validRecord(defaults, identity));
  assert(defaults.rx1Delay == 1);
  assert(defaults.rx1DrOffset == 0);
  assert(defaults.rx2DataRateIndex == 0);
  assert(defaults.rx2Frequency == 869525000);
  assert(defaults.rx2Answer == 0 && defaults.rxTimingAnswer == 0);

  const State::Record negotiated = State::makeRecord(identity, 5, 0, 3,
                                                     869525000, 0xC7, 2);
  // Fixture CRC computed independently with Python struct.pack and zlib.crc32.
  assert(negotiated.crc == UINT32_C(0x2c0c313f));
  uint8_t bytes[State::ENCODED_SIZE];
  State::encodeRecord(negotiated, bytes);
  assert(bytes[8] == 0xef && bytes[15] == 0x01); // Identity is little-endian.
  assert(bytes[16] == 5 && bytes[17] == 0 && bytes[18] == 3);
  assert(bytes[19] == 0xC7 && bytes[20] == 2);
  assert(bytes[21] == 0 && bytes[22] == 0 && bytes[23] == 0);
  State::Record decoded = {};
  assert(State::decodeRecord(bytes, sizeof(bytes), decoded));
  assert(State::validRecord(decoded, identity));
  assert(!State::validRecord(decoded, identity ^ 1));
  assert(decoded.rx1Delay == 5 && decoded.rx2DataRateIndex == 3);
  assert(decoded.rx2Frequency == 869525000);
  uint8_t secondEncoding[State::ENCODED_SIZE];
  State::encodeRecord(decoded, secondEncoding);
  assert(std::memcmp(bytes, secondEncoding, sizeof(bytes)) == 0);

  // Simulate reboot using the persisted bytes, never a raw in-memory struct.
  State::Record rebooted = {};
  assert(State::decodeRecord(secondEncoding, sizeof(secondEncoding), rebooted));
  assert(State::validRecord(rebooted, identity));
  assert(rebooted.rx1Delay == 5 && rebooted.rx2DataRateIndex == 3);
  assert(rebooted.rx2Answer == 0xC7 && rebooted.rxTimingAnswer == 2);

  // The caller can normalize these sticky responses after reboot so settings
  // are acknowledged again instead of silently losing pending negotiation.
  rebooted.rx2Answer &= static_cast<uint8_t>(~0x40);
  if (rebooted.rxTimingAnswer == 2) rebooted.rxTimingAnswer = 1;
  rebooted.crc = State::recordChecksum(rebooted);
  assert(State::validRecord(rebooted, identity));
  assert(rebooted.rx2Answer == 0x87 && rebooted.rxTimingAnswer == 1);

  // Every single stored byte is protected, including the stored CRC.
  for (uint32_t i = 0; i < State::ENCODED_SIZE; ++i) {
    uint8_t corrupt[State::ENCODED_SIZE];
    std::memcpy(corrupt, bytes, sizeof(corrupt));
    corrupt[i] ^= 1;
    State::Record output = defaults;
    assert(!State::decodeRecord(corrupt, sizeof(corrupt), output));
    assert(State::validRecord(output, identity));
    assert(output.rx1Delay == defaults.rx1Delay);
  }
  assert(!State::decodeRecord(nullptr, sizeof(bytes), decoded));
  assert(!State::decodeRecord(bytes, sizeof(bytes) - 1, decoded));
  assert(!State::decodeRecord(bytes, sizeof(bytes) + 1, decoded));

  State::Record bad = negotiated;
  ++bad.magic;
  expectRejected(bad);
  bad = negotiated;
  ++bad.version;
  expectRejected(bad);
  bad = negotiated;
  bad.reserved = 1;
  expectRejected(bad);
  bad = negotiated;
  bad.reserved2 = 1;
  expectRejected(bad);
  bad = negotiated;
  bad.rx1Delay = 0;
  expectRejected(bad);
  bad.rx1Delay = 16;
  expectRejected(bad);
  bad = negotiated;
  bad.rx1DrOffset = 4;
  expectRejected(bad);
  bad = negotiated;
  bad.rx2DataRateIndex = 7;
  expectRejected(bad);
  bad = negotiated;
  bad.rx2Frequency = 862999999;
  expectRejected(bad);
  bad.rx2Frequency = 870000001;
  expectRejected(bad);
  bad = negotiated;
  bad.rx2Answer = 0x07; // Response flags are required for a pending answer.
  expectRejected(bad);
  bad.rx2Answer = 0x40;
  expectRejected(bad);
  bad.rx2Answer = 0x88;
  expectRejected(bad);
  bad.rx2Answer = 0xC8;
  expectRejected(bad);
  bad = negotiated;
  bad.rxTimingAnswer = 3;
  expectRejected(bad);

  assert(State::validRecord(State::makeRecord(identity, 15, 3, 6, 863000000)));
  assert(State::validRecord(State::makeRecord(identity, 1, 0, 0, 870000000)));
  assert(!State::validRecord(State::makeRecord(identity, 0)));
  assert(!State::validRecord(State::makeRecord(identity, 16)));
  assert(!State::validRecord(State::makeRecord(identity, 257)));
  assert(!State::validRecord(State::makeRecord(identity, 5, 256)));
  assert(!State::validRecord(State::makeRecord(identity, 5, 0, 256)));
  assert(!State::validRecord(State::makeRecord(identity, 5, 0, 3, 0)));
  assert(!State::validRecord(State::makeRecord(identity, 5, 0, 3, 869525000, 256)));
  assert(!State::validRecord(State::makeRecord(identity, 5, 0, 3, 869525000, 0, 256)));
  for (uint32_t answer = 0; answer <= 7; ++answer) {
    assert(State::validRecord(State::makeRecord(identity, 5, 0, 3,
                                                869525000, 0x80 | answer, 1)));
    assert(State::validRecord(State::makeRecord(identity, 5, 0, 3,
                                                869525000, 0xC0 | answer, 2)));
  }
  std::puts("PASS: receive-state defaults, persistence, integrity, identity, and bounds.");
}
