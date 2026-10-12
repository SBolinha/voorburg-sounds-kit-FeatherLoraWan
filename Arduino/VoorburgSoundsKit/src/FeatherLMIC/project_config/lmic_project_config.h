#pragma once

// Feather RP2040 RFM95 on the European TTN frequency plan.
#define CFG_eu868 1
#define CFG_sx1276_radio 1
#define LMIC_DEBUG_LEVEL 0

// This device uses an existing ABP session and Class A receive windows.
// Keep all Class A MAC commands and DeviceTime, remove unused join/Class B code.
#define DISABLE_JOIN
#define DISABLE_PING
#define DISABLE_BEACONS
