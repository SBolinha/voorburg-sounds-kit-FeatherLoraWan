# Bundled MCCI LoRaWAN LMIC 5.0.1

Source: the official Arduino Library Manager distribution of
[MCCI LoRaWAN LMIC 5.0.1](https://github.com/mcci-catena/arduino-lmic/tree/v5.0.1).
The original library metadata and license are included.

Archive SHA-256:
`465c172306ff6772e8665675f4b7f50e2d2091eacab2a079d4d505792bf01952`.

This copy is compiled as part of the Feather sketch's `src` directory. It fixes
the dependency version and region independently of global Arduino libraries.
LMIC 6.0.1 signs and encrypts ABP packets with unset default-backend keys;
5.0.1 uses the keys supplied to `LMIC_setSession`.

Local changes to the upstream distribution:

- `project_config/lmic_project_config.h` selects EU868, SX1276 and debug level 0,
  and excludes unused OTAA join and Class B beacon/ping code. Class A MAC
  configuration and DeviceTime request/answer support remain enabled.
- `src/hal/hal.h` and `src/hal/getpinmap*.cpp` use relative paths for internal
  headers so the sources compile inside the sketch without a library include path.
- The unused certification-test helper `lmic_compliance.c` is omitted. It
  assumes joining is enabled, whereas this device uses only ABP.

The protocol and crypto source files are unchanged. The sketch supplies the
Feather RP2040 RFM pin map and checks the loaded session keys before transmitting.

Validate the actual bundled packet builder from the repository root with:

```sh
node Tools/test_feather_session_crypto.mjs Arduino/AmsterdamSoundsKit/src/FeatherLMIC --build-dir /tmp/feather-session-crypto
```
