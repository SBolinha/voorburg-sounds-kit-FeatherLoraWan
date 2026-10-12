import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const context = vm.createContext({});
vm.runInContext(readFileSync(new URL('./feather_ttn_uplink_decoder.js', import.meta.url), 'utf8'), context);
const decode = (hex, fPort = 1) => JSON.parse(JSON.stringify(
  context.decodeUplink({ bytes: [...Buffer.from(hex, 'hex')], fPort })));
const actual = { mean: 35, min: 29.5, max: 76, stddev: 4.5, leq: 44.5 };
assert.deepEqual(decode('463B982459'), { data: actual });
assert.deepEqual(decode('463B982459E808'), { data: { ...actual, battery_adc: 2280 } });
assert.deepEqual(decode('4A3D7C2754763AC96A'), {
  data: { mean: 37, min: 30.5, max: 62, stddev: 4.875, leq: 42, at: '2026-10-09T19:03:18.000Z' }
});
assert.deepEqual(decode('4A3D7C2754763AC96AFF0F'), {
  data: { mean: 37, min: 30.5, max: 62, stddev: 4.875, leq: 42,
    at: '2026-10-09T19:03:18.000Z', battery_adc: 4095 }
});
assert.equal(decode('000000000001000080').data.at, '2038-01-19T03:14:09.000Z');
assert.equal(decode('0000000000FFFFFFFF').data.at, '2106-02-07T06:28:15.000Z');
assert.equal(decode('463B9824590000').data.battery_adc, 0);
for (const hex of ['', '00', '000000000000', '0000000000000000', '00000000000000000000'])
  assert(decode(hex).errors?.length);
assert(decode('463B982459', 0).errors?.length);
assert(decode('463B98245900000000').errors?.length);
assert(decode('463B9824590010').errors?.length);
assert(decode('463B982459763AC96A0010').errors?.length);
console.log('PASS: formatter decodes all four payload layouts, the real TTN sample, unsigned UTC and raw ADC boundaries; malformed data rejected.');
