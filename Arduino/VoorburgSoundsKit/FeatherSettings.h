#pragma once
#include <stdint.h>

// Increment for every firmware rebuild that is uploaded to the Feather.
#define FEATHER_FIRMWARE_VERSION "v13"

// Use the Pico-style split: core 1 owns I2S/DSP, core 0 owns LMIC/storage.
#define FEATHER_DUAL_CORE 1

// This firmware belongs to TTN end device lmic-spoorbrug. EUIs are metadata
// for ABP; the actual data-frame identity is DEVADDR plus the session keys.
#define FEATHER_EXPECTED_DEVADDR 0x01020304UL
#define FEATHER_NET_ID 0x000013UL
static const uint8_t FEATHER_DEVEUI_LSB[8] = {
  0, 0, 0, 0, 0, 0, 0, 0
}; // Replace with the private device EUI in LMIC little-endian order.

// No receive-state file yet: match TTN's actual initial ABP parameters.
// Received MAC commands negotiate the later five-second/SF9 settings; the
// firmware saves and restores them separately from its existing counters.
#define FEATHER_INITIAL_RX1_DELAY_SECONDS 1
#define FEATHER_INITIAL_RX2_DATA_RATE_INDEX 0 // SF12/BW125

// GPS-UTC is 18 seconds in 2026 (IERS Bulletin C 72: UTC-TAI remains -37).
// https://datacenter.iers.org/data/html/bulletinc-072.html
#define FEATHER_GPS_UTC_OFFSET_SECONDS 18UL
#define FEATHER_CLOCK_RESYNC_SECONDS 21600UL

// Normal logging matches the Pico's raw PAYLOAD_HEX line. Radio diagnostics
// can be enabled temporarily without changing the application payload.
#ifndef USB_RADIO_LOGGING
#define USB_RADIO_LOGGING 0
#endif

// New ABP session only. Existing sessions resume their persisted counter block.
// If recovering a session whose local state was deliberately erased, set this
// above the last FCntUp accepted by TTN before rebuilding.
#define FEATHER_INITIAL_FCNT_UP 0UL

// Normal operation uses five-minute summaries; USB diagnostics remain enabled
// through the existing USB_LOGGING setting in the private LoraSettings.h.
#define FEATHER_MEASUREMENT_SECONDS 300UL

// External divider installed: BAT -- 10k -- A0 -- 10k -- GND.
// Disable sensing if the divider is removed; never connect BAT directly to A0.
// The payload carries the unconverted 12-bit ADC count, not a charge estimate.
#ifndef FEATHER_BATTERY_ENABLED
#define FEATHER_BATTERY_ENABLED 1
#endif
#define FEATHER_BATTERY_ADC_PIN A0
