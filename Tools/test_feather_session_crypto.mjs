// Run against an actual, locally installed/downloaded MCCI LMIC source tree:
// node Tools/test_feather_session_crypto.mjs /path/to/LMIC --build-dir /path/to/work/build
// For 6.0.1, add --expect-zero-regression to reproduce its ABP key-store defect.
// This never reads LoraSettings.h, opens a serial port, or uses the network.
import assert from 'node:assert/strict';
import { createCipheriv } from 'node:crypto';
import { existsSync, mkdirSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const args = process.argv.slice(2);
const library = path.resolve(args.shift() ?? '');
const regression = args.includes('--expect-zero-regression');
const buildFlag = args.indexOf('--build-dir');
const build = path.resolve(buildFlag < 0 ? 'work/feather-session-host' : args[buildFlag + 1]);
assert(existsSync(path.join(library, 'src/lmic/lmic.c')), 'Pass an actual MCCI LMIC source tree');
mkdirSync(build, { recursive: true });
const toolsDirectory = path.dirname(fileURLToPath(import.meta.url));
const nwk = Buffer.from('2b7e151628aed2a6abf7158809cf4f3c', 'hex');
const app = Buffer.from('000102030405060708090a0b0c0d0e0f', 'hex');

function aes(key, block) {
  const cipher = createCipheriv('aes-128-ecb', key, null);
  cipher.setAutoPadding(false);
  return Buffer.concat([cipher.update(block), cipher.final()]);
}
function xor(a, b) { return Buffer.from(a.map((value, i) => value ^ b[i])); }
function double(block) {
  const result = Buffer.alloc(16);
  for (let i = 0; i < 16; ++i) result[i] = (block[i] << 1) | (i < 15 ? block[i + 1] >>> 7 : 0);
  if (block[0] & 0x80) result[15] ^= 0x87;
  return result;
}
// RFC4493 CMAC built on Node/OpenSSL AES, independent of LMIC's AES/CMAC.
function cmac(key, message) {
  const first = double(aes(key, Buffer.alloc(16)));
  const second = double(first);
  const count = Math.max(1, Math.ceil(message.length / 16));
  const last = Buffer.alloc(16);
  message.copy(last, 0, (count - 1) * 16);
  const complete = message.length > 0 && message.length % 16 === 0;
  if (!complete) last[message.length % 16] = 0x80;
  let state = Buffer.alloc(16);
  for (let i = 0; i < count - 1; ++i) state = aes(key, xor(state, message.subarray(i * 16, i * 16 + 16)));
  return aes(key, xor(state, xor(last, complete ? first : second)));
}
// Validate the independent primitive against the NIST/RFC4493 examples.
assert.equal(aes(nwk, Buffer.from('6bc1bee22e409f96e93d7e117393172a', 'hex')).toString('hex'),
  '3ad77bb40d7a3660a89ecaf32466ef97');
assert.equal(cmac(nwk, Buffer.alloc(0)).toString('hex'), 'bb1d6929e95937287fa37d129b756746');
assert.equal(cmac(nwk, Buffer.from('6bc1bee22e409f96e93d7e117393172a', 'hex')).toString('hex'),
  '070a16b46b4d4144f79bdd9dd04a287c');

function referenceFrame(counter, port, clear, nwkKey = nwk, appKey = app) {
  const header = Buffer.from('400403020100000000', 'hex');
  header.writeUInt16LE(counter & 0xffff, 6);
  header[8] = port;
  const encrypted = Buffer.from(clear);
  for (let offset = 0; offset < encrypted.length; offset += 16) {
    const a = Buffer.alloc(16);
    a[0] = 1;
    a.writeUInt32LE(0x01020304, 6);
    a.writeUInt32LE(counter, 10);
    a[15] = offset / 16 + 1;
    const stream = aes(port === 0 ? nwkKey : appKey, a);
    for (let i = 0; i < Math.min(16, encrypted.length - offset); ++i) encrypted[offset + i] ^= stream[i];
  }
  const message = Buffer.concat([header, encrypted]);
  const b0 = Buffer.alloc(16);
  b0[0] = 0x49;
  b0.writeUInt32LE(0x01020304, 6);
  b0.writeUInt32LE(counter, 10);
  b0[15] = message.length;
  return Buffer.concat([message, cmac(nwkKey, Buffer.concat([b0, message])).subarray(0, 4)]);
}

function run(command, arguments_) {
  const result = spawnSync(command, arguments_, { encoding: 'utf8' });
  assert.equal(result.status, 0, `${command} failed:\n${result.stdout}\n${result.stderr}`);
  return result.stdout;
}
const flags = ['-Os', '-ffunction-sections', '-fdata-sections', '-DARDUINO_LMIC_PROJECT_CONFIG_H_SUPPRESS',
  '-DCFG_eu868=1', '-DCFG_sx1276_radio=1', '-DDISABLE_JOIN', '-DDISABLE_BEACONS', '-DDISABLE_PING',
  '-I', path.join(library, 'src')];
const cSources = [path.join(toolsDirectory, 'lmic_host_frame.c'),
  ...['aes/other.c', 'lmic/lmic_eu868.c', 'lmic/lmic_eu_like.c'].map(file => path.join(library, 'src', file))];
// The separate secure-element driver was introduced in 6.x.
const secureElement = path.join(library, 'src/se/drivers/default/lmic_se_default.c');
if (existsSync(secureElement)) cSources.push(secureElement);
const objects = [];
for (let i = 0; i < cSources.length; ++i) {
  const object = path.join(build, `source-${i}.o`);
  run(process.env.CC ?? 'cc', ['-std=c11', ...flags, '-c', cSources[i], '-o', object]);
  objects.push(object);
}
const aesObject = path.join(build, 'aes.o');
run(process.env.CXX ?? 'c++', [...flags, '-c', path.join(library, 'src/aes/ideetron/AES-128_V10.cpp'), '-o', aesObject]);
objects.push(aesObject);
const executable = path.join(build, 'lmic-host-frame');
run(process.env.CXX ?? 'c++', [...objects,
  process.platform === 'darwin' ? '-Wl,-dead_strip' : '-Wl,--gc-sections', '-o', executable]);

const cases = [
  { counter: 256, port: 1, payload: Buffer.from('0123456789', 'hex') },
  { counter: 65536, port: 1, payload: Buffer.from('000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20', 'hex') },
  { counter: 257, port: 0, payload: Buffer.from('0102030405060708090a0b0c0d0e0f1011', 'hex') },
  // Pico's public sample: five acoustic values followed by unsigned UTC
  // window-end seconds, little-endian. Exercise the full 32-bit FCntUp.
  { counter: 65537, port: 1, payload: Buffer.from('8F4EA51E92009FC86A', 'hex') },
  { counter: 1024, port: 1, payload: Buffer.from('463B982459E808', 'hex') },
  { counter: 1025, port: 1, payload: Buffer.from('8F4EA51E92009FC86AE808', 'hex') },
];
assert.equal(cases[3].payload.length, 9);
assert.equal(cases[3].payload.readUInt32LE(5), 0x6ac89f00);
for (const fixture of cases) {
  const output = run(executable, [String(fixture.counter), String(fixture.port), fixture.payload.toString('hex')]);
  const values = Object.fromEntries(output.trim().split('\n').map(line => line.split('=')));
  assert.equal(values.keys_match, regression ? '0' : '1', 'LMIC_getSessionKeys must reflect the actual encoder key storage');
  const actual = Buffer.from(values.frame, 'hex');
  const expected = referenceFrame(fixture.counter, fixture.port, fixture.payload);
  assert.equal(values.next_counter, String(fixture.counter + 1));
  if (regression) {
    const zero = Buffer.alloc(16);
    assert.notEqual(actual.toString('hex'), expected.toString('hex'), 'Broken ABP packet must differ from configured-key reference');
    assert.deepEqual(actual, referenceFrame(fixture.counter, fixture.port, fixture.payload, zero, zero),
      'LMIC 6.0.1 ABP regression must reproduce all-zero session-key encryption and MIC');
  } else {
    assert.deepEqual(actual, expected, `Configured-key uplink mismatch: FCntUp=${fixture.counter}, FPort=${fixture.port}`);
  }
}
console.log(regression ? `PASS: actual LMIC reproduces zero-key ABP regression in ${cases.length} fixtures.` :
  `PASS: actual LMIC configured-key encryption, MIC, key readback, and frame counters match ${cases.length} independent fixtures, including 5/7/9/11-byte application payloads and a MAC-only FPort 0 reply.`);
