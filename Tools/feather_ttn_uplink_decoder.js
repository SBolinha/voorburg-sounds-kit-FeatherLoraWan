// TTN v3 uplink formatter: FPort 1, five measurement bytes plus an optional
// unsigned UTC window-end timestamp in little-endian bytes 5..8.
// Optional final two bytes contain the unconverted 12-bit battery ADC count.
// mean/min/max/leq are in dBA; stddev is in dB.
function decodeUplink(input) {
  var bytes = input.bytes || [];
  if (input.fPort !== 1) {
    return { errors: ["Feather measurement summaries use FPort 1."] };
  }
  if (bytes.length !== 5 && bytes.length !== 7 && bytes.length !== 9 && bytes.length !== 11) {
    return { errors: ["Expected 5/7 bytes without UTC or 9/11 bytes with UTC."] };
  }
  var data = {
    mean: bytes[0] / 2,
    min: bytes[1] / 2,
    max: bytes[2] / 2,
    stddev: bytes[3] / 8,
    leq: bytes[4] / 2
  };
  if (bytes.length === 9 || bytes.length === 11) {
    // Arithmetic keeps post-2038 timestamps unsigned, matching the Pico.
    var seconds = bytes[5] + bytes[6] * 256 + bytes[7] * 65536 + bytes[8] * 16777216;
    if (seconds === 0) return { errors: ["Measurement UTC timestamp is missing."] };
    data.at = new Date(seconds * 1000).toISOString();
  }
  if (bytes.length === 7 || bytes.length === 11) {
    var offset = bytes.length - 2;
    var adc = bytes[offset] + bytes[offset + 1] * 256;
    if (adc > 4095) return { errors: ["Battery ADC count exceeds its 12-bit range."] };
    data.battery_adc = adc;
  }
  return { data: data };
}
