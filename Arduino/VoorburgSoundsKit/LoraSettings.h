/*

 LORA region configuration can be found here : Arduino/libraries/MCCI_LoRaWAN_LMIC_library/project_config 
 Currently uses ABP mode only. OTAA still needs some work.
 
*/

#if defined(ARDUINO_ARCH_SAMD) && !defined(ARDUINO_SAMD_FEATHER_M0)
  // This codebase's legacy SAMD pin map is specifically for the Feather M0
  // RFM95. The explicit macro selects its built-in MCCI LMIC pin map.
  #define ARDUINO_SAMD_FEATHER_M0 1
#endif

#include "src/FeatherLMIC/src/arduino_lmic_hal_boards.h"

#define ABP_MODE 1

// Set to 1 to enable USB Serial diagnostics. The device waits up to five
// seconds for the Serial Monitor, then continues even if it is not connected.
// Set to 0 for silent operation using SerialDummy.
#ifndef USB_LOGGING
  #define USB_LOGGING 1
#endif

// The radio is the same SX127x family on both Feather models, but the
// RP2040 board wires it to different GPIOs. Keep an explicit pin map so the
// project does not depend on LMIC's board autodetection.
#if defined(ARDUINO_ADAFRUIT_FEATHER_RP2040_RFM)
  #define LORA_CS_PIN 16
  static const lmic_pinmap featherRp2040RfmPinMap = {
    16,                 // nss / chip select
    LMIC_UNUSED_PIN,    // rxtx
    17,                 // reset
    {21, 22, 23},       // DIO0, DIO1, DIO2
    0,                  // rxtx_rx_active
    0,                  // rssi_cal
    0                   // spi_freq: use the LMIC default
  };
  const lmic_pinmap *pPinMap = &featherRp2040RfmPinMap;
#elif defined(ARDUINO_ARCH_SAMD)
  #define LORA_CS_PIN 8
  const lmic_pinmap *pPinMap = Arduino_LMIC::GetPinmap_FeatherM0LoRa();
#else
  #error "Select Adafruit Feather RP2040 RFM or Feather M0 LoRa in the Arduino IDE."
#endif

#ifdef ABP_MODE
 
  // Feather-specific ABP keys and address; keys use TTN byte order.
  // Publication placeholder. Copy real values from TTN into a private local
  // LoraSettings.h; never commit session keys or a live device address.
  static const PROGMEM u1_t NWKSKEY[16] = {0};
  static const u1_t PROGMEM APPSKEY[16] = {0};
  static const u4_t DEVADDR = 0x01020304;

  // Identity callbacks are initialized explicitly in VoorburgSoundsKit.ino.
#else

  
#endif
