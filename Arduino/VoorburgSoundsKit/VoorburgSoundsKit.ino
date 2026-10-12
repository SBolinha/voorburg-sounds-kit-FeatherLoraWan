/**

 Amsterdam Sounds Kit dBA sound level meter for TTN.

*/

#include "src/FeatherLMIC/src/lmic.h"
#include "src/FeatherLMIC/src/hal/hal.h"
#include <LittleFS.h>
#include "SLM.h"
#include "LoraSettings.h"
#include "SerialDummy.h"
#include "FeatherSettings.h"
#include "FeatherCounterState.h"
#include "FeatherReceiveState.h"
#include "FeatherMeasurementPayload.h"
#include "FeatherUtcClock.h"
#include "FeatherEventLog.h"
#include <pico/time.h>
#include <sys/time.h>

#if !defined(ARDUINO_ADAFRUIT_FEATHER_RP2040_RFM)
#error "Select Adafruit Feather RP2040 RFM for this firmware."
#endif
#if !defined(CFG_eu868) || !defined(CFG_sx1276_radio)
#error "The bundled Feather LMIC configuration must select EU868 and SX1276."
#endif
#if ARDUINO_LMIC_VERSION != ARDUINO_LMIC_VERSION_CALC(5, 0, 1, 0)
#error "The Feather requires its bundled LMIC 5.0.1: LMIC 6.0.1 does not load ABP session keys into its crypto backend."
#endif
static_assert(DEVADDR == FEATHER_EXPECTED_DEVADDR,
              "LoraSettings.h and FeatherSettings.h must use the same device address.");

// ABP never joins. Initialize the identity callbacks completely anyway;
// AppEUI and the unused OTAA key are explicitly absent, not uninitialized.
void os_getArtEui(u1_t *buf) { memset(buf, 0, 8); }
void os_getDevEui(u1_t *buf) { memcpy(buf, FEATHER_DEVEUI_LSB, 8); }
void os_getDevKey(u1_t *buf) { memset(buf, 0, 16); }

// Send to TTN after each N seconds of measuring
#define SEND_AFTER    FEATHER_MEASUREMENT_SECONDS

#if USB_LOGGING
FeatherEventLog eventLog;
#define LOG_PRINT(...) eventLog.print(__VA_ARGS__)
#define LOG_PRINTLN(...) eventLog.println(__VA_ARGS__)
#else
#define LOG_PRINT(...) do {} while (0)
#define LOG_PRINTLN(...) do {} while (0)
#endif

#if !USB_LOGGING
#define Serial      dummy
#endif

// Sound level meter
SLM slm(Serial);
// These flags are read by both RP2040 cores. Volatile is required because
// core 1 may start before setup() on core 0 has published the initial state.
static volatile bool recording = false;
static bool radioReady = false;
static bool transmissionComplete = false;
static volatile bool halted = false;
static volatile bool transportRecoveryRequested = false;
static volatile bool audioBootstrapReady = false;
static volatile uint32_t audioHeartbeatMs = 0;
static volatile uint32_t audioPauseRequest = 0;
static volatile uint32_t audioPauseAck = 0;
static volatile bool audioPaused = false;
static uint32_t audioPauseGeneration = 0;
static volatile uint64_t measurementStartedUs = 0;
static volatile uint64_t measurementWindowEndUs = 0;
#if defined(ARDUINO_ARCH_RP2040) && FEATHER_DUAL_CORE
// Core-1 scheduler deadline: uint32_t reads/writes are atomic on RP2040.
static volatile uint32_t measurementWindowEndMs = 0;
#endif
static bool measurementHasStarted = false;
static FeatherCounterState counters;
static FeatherUtcClock utcClock;
static uint64_t sessionIdentityTag = 0;
static FeatherReceiveState::Record receiveRecord = {};
static bool receiveRecordLoaded = false;
static bool timeFailureReported = false;
static uint8_t rx1IrqFlags = 0;
static bool rx2Opened = false;
static uint32_t rxLateCountAtTx = 0;
static uint32_t rxLateTicksAtTx = 0;

void connect();

struct FeatherMeasurementSnapshot {
  double mean;
  double minimum;
  double maximum;
  double stddev;
  double leq;
  uint64_t windowEndUs;
};
#if defined(ARDUINO_ARCH_RP2040) && FEATHER_DUAL_CORE
// Core 1 completes one acoustic window while core 0 services LMIC. The flag
// is published only after the complete snapshot has been written.
static FeatherMeasurementSnapshot measurementSnapshot = {};
static volatile bool measurementSnapshotReady = false;
#endif

// Verify the same key storage that LMIC 5.0.1 uses for packet encryption and
// authentication. Repeat before each measurement transmission after I2S work.
static bool sessionKeysMatch()
{
  uint8_t expectedNwk[16], expectedApp[16], loadedNwk[16], loadedApp[16];
  memcpy_P(expectedNwk, NWKSKEY, sizeof(expectedNwk));
  memcpy_P(expectedApp, APPSKEY, sizeof(expectedApp));
  u4_t loadedNetId;
  devaddr_t loadedAddress;
  LMIC_getSessionKeys(&loadedNetId, &loadedAddress, loadedNwk, loadedApp);
  return loadedNetId == FEATHER_NET_ID && loadedAddress == DEVADDR &&
         memcmp(expectedNwk, loadedNwk, sizeof(expectedNwk)) == 0 &&
         memcmp(expectedApp, loadedApp, sizeof(expectedApp)) == 0;
}

extern uint8_t _FS_start;
extern uint8_t _FS_end;

static void failClosed(const __FlashStringHelper *message)
{
  LOG_PRINTLN(message);
  halted = true;
  recording = false;
  digitalWrite(LED_BUILTIN, LOW);
  LMIC_shutdown();
  slm.stop();
}

static bool readCounterRecord(const char *path, FeatherCounterState::Record &record)
{
  File file = LittleFS.open(path, "r");
  if (!file || file.size() != FeatherCounterState::ENCODED_SIZE) return false;
  uint8_t bytes[FeatherCounterState::ENCODED_SIZE];
  size_t n = file.read(bytes, sizeof(bytes));
  file.close();
  return FeatherCounterState::decodeRecord(bytes, n, record);
}

#if defined(ARDUINO_ARCH_RP2040) && FEATHER_DUAL_CORE
static bool pauseAudioForFlash()
{
  uint32_t generation = ++audioPauseGeneration;
  if (!generation) generation = ++audioPauseGeneration;
  audioPauseAck = 0;
  audioPauseRequest = generation;
  __dmb();
  const uint32_t started = millis();
  while (audioPauseAck != generation && millis() - started < 250) delay(1);
  if (audioPauseAck != generation) {
    audioPauseRequest = 0;
    return false;
  }
  audioPaused = true;
  return true;
}

static void resumeAudioAfterFlash()
{
  audioPauseRequest = 0;
  audioPaused = false;
  __dmb();
}
#endif

// LittleFS rename preserves the previous committed record if power fails
// before the new record is committed. Flash writes happen with I2S stopped.
static bool writeCounterRecord(const FeatherCounterState::Record &record, void *)
{
#if defined(ARDUINO_ARCH_RP2040) && FEATHER_DUAL_CORE
  // LittleFS writes erase/program flash. Park the audio core while code or
  // data may otherwise be executing from XIP flash.
  const bool parkedAudioCore = audioBootstrapReady;
  if (parkedAudioCore && !pauseAudioForFlash()) return false;
#endif
  bool result = false;
  uint8_t bytes[FeatherCounterState::ENCODED_SIZE];
  FeatherCounterState::encodeRecord(record, bytes);
  File file = LittleFS.open("/feather-counters.new", "w");
  if (file) {
    size_t n = file.write(bytes, sizeof(bytes));
    file.flush();
    file.close();
    FeatherCounterState::Record check;
    if (n == sizeof(bytes) && readCounterRecord("/feather-counters.new", check) &&
        check.crc == record.crc &&
        LittleFS.rename("/feather-counters.new", "/feather-counters.bin"))
      result = readCounterRecord("/feather-counters.bin", check) && check.crc == record.crc;
  }
#if defined(ARDUINO_ARCH_RP2040) && FEATHER_DUAL_CORE
  if (parkedAudioCore) resumeAudioAfterFlash();
#endif
  return result;
}

static bool startCounterStore(uint64_t identityTag)
{
  const uintptr_t start = reinterpret_cast<uintptr_t>(&_FS_start);
  const uintptr_t end = reinterpret_cast<uintptr_t>(&_FS_end);
  if (end <= start || end - start < 65536) return false;
  LittleFS.setConfig(LittleFSConfig(false));
  if (!LittleFS.begin())
  {
    // Provision only untouched storage. Never format corrupt session state.
    const uint8_t *flash = reinterpret_cast<const uint8_t *>(start);
    for (size_t i = 0; i < end - start; ++i)
      if (flash[i] != 0xFF) return false;
    if (!LittleFS.format() || !LittleFS.begin()) return false;
  }
  const bool exists = LittleFS.exists("/feather-counters.bin");
  FeatherCounterState::Record record = {};
  if (exists && !readCounterRecord("/feather-counters.bin", record)) return false;
  return counters.begin(identityTag, FEATHER_INITIAL_FCNT_UP, exists, record,
                        writeCounterRecord);
}

static bool readReceiveRecord(const char *path, FeatherReceiveState::Record &record)
{
  File file = LittleFS.open(path, "r");
  if (!file || file.size() != FeatherReceiveState::ENCODED_SIZE) return false;
  uint8_t bytes[FeatherReceiveState::ENCODED_SIZE];
  const size_t n = file.read(bytes, sizeof(bytes));
  file.close();
  return FeatherReceiveState::decodeRecord(bytes, n, record) &&
         FeatherReceiveState::validRecord(record, sessionIdentityTag);
}

static bool writeReceiveRecord(const FeatherReceiveState::Record &record)
{
  if (!FeatherReceiveState::validRecord(record, sessionIdentityTag)) return false;
#if defined(ARDUINO_ARCH_RP2040) && FEATHER_DUAL_CORE
  const bool parkedAudioCore = audioBootstrapReady;
  if (parkedAudioCore && !pauseAudioForFlash()) return false;
#endif
  bool result = false;
  uint8_t bytes[FeatherReceiveState::ENCODED_SIZE];
  FeatherReceiveState::encodeRecord(record, bytes);
  File file = LittleFS.open("/feather-receive.new", "w");
  if (file) {
    const size_t n = file.write(bytes, sizeof(bytes));
    file.flush();
    file.close();
    FeatherReceiveState::Record check;
    if (n == sizeof(bytes) && readReceiveRecord("/feather-receive.new", check) &&
        check.crc == record.crc &&
        LittleFS.rename("/feather-receive.new", "/feather-receive.bin"))
      result = readReceiveRecord("/feather-receive.bin", check) && check.crc == record.crc;
  }
#if defined(ARDUINO_ARCH_RP2040) && FEATHER_DUAL_CORE
  if (parkedAudioCore) resumeAudioAfterFlash();
#endif
  return result;
}

static bool startReceiveStore()
{
  const bool exists = LittleFS.exists("/feather-receive.bin");
  if (exists) {
    if (!readReceiveRecord("/feather-receive.bin", receiveRecord)) return false;
  } else {
    // Match the actual initial TTN settings seen in the device export. The
    // server negotiates its desired five-second/SF9 settings over the radio.
    receiveRecord = FeatherReceiveState::makeRecord(sessionIdentityTag,
        FEATHER_INITIAL_RX1_DELAY_SECONDS, 0,
        FEATHER_INITIAL_RX2_DATA_RATE_INDEX, 869525000);
    if (!writeReceiveRecord(receiveRecord)) return false;
  }
  receiveRecordLoaded = true;
  LMIC.rxDelay = receiveRecord.rx1Delay;
  LMIC.rx1DrOffset = receiveRecord.rx1DrOffset;
  LMIC.dn2Dr = receiveRecord.rx2DataRateIndex;
  LMIC.dn2Freq = receiveRecord.rx2Frequency;
  // A reboot may have interrupted the immediate MAC answer. Repeat sticky
  // answers instead of skipping a first copy that was lost with RAM.
  LMIC.dn2Ans = receiveRecord.rx2Answer & ~0x40;
  LMIC.macRxTimingSetupAns = receiveRecord.rxTimingAnswer ? 1 : 0;
  return true;
}

static bool saveReceiveState()
{
  const FeatherReceiveState::Record current = FeatherReceiveState::makeRecord(
      sessionIdentityTag, LMIC.rxDelay, LMIC.rx1DrOffset, LMIC.dn2Dr,
      LMIC.dn2Freq, LMIC.dn2Ans, LMIC.macRxTimingSetupAns);
  if (!FeatherReceiveState::validRecord(current, sessionIdentityTag)) return false;
  if (receiveRecordLoaded && current.crc == receiveRecord.crc) return true;
  if (!writeReceiveRecord(current)) return false;
  receiveRecord = current;
  receiveRecordLoaded = true;
  return true;
}

static void networkTimeReceived(void *, int success)
{
  lmic_time_reference_t reference;
  const uint64_t nowUs = time_us_64();
  // getNetworkTimeReference can retain an earlier reply; success must apply
  // to this request. Unsigned subtraction handles LMIC tick wrap safely.
  if (success && LMIC_getNetworkTimeReference(&reference)) {
    const uint32_t elapsed = uint32_t(os_getTime()) - uint32_t(reference.tLocal);
    if (utcClock.synchronize(reference.tNetwork, elapsed, OSTICKS_PER_SEC,
                             nowUs, FEATHER_GPS_UTC_OFFSET_SECONDS)) {
      uint64_t utcUs;
      if (utcClock.microsAt(nowUs, utcUs)) {
        struct timeval tv;
        tv.tv_sec = utcUs / 1000000ULL;
        tv.tv_usec = utcUs % 1000000ULL;
        settimeofday(&tv, nullptr);
        timeFailureReported = false;
        LOG_PRINTLN(F("UTC synchronized over LoRaWAN; measurements now include their window-end timestamp"));
        return;
      }
    }
  }
  if (!timeFailureReported) {
    uint32_t runningUtc;
    if (utcClock.secondsAt(nowUs, runningUtc))
      LOG_PRINTLN(F("LoRaWAN time refresh unanswered; keeping the synchronized running clock"));
    else
      LOG_PRINTLN(F("LoRaWAN time unavailable; payloads omit UTC until the clock is synchronized"));
#if USB_LOGGING
    // One diagnostic after an unanswered time request, outside the receive
    // windows. Preserve RX1's IRQ flags before RX2 overwrites them so a
    // received-but-rejected frame differs from a radio timeout.
    LOG_PRINT(F("RX diagnostic: IRQ1=0x"));
    LOG_PRINT(rx2Opened ? rx1IrqFlags : LMIC.saveIrqFlags, HEX);
    LOG_PRINT(F(", IRQ2=0x"));
    LOG_PRINT(rx2Opened ? LMIC.saveIrqFlags : 0, HEX);
    LOG_PRINT(F(", late windows="));
    LOG_PRINT(uint32_t(LMIC.radio.rxlate_count) - rxLateCountAtTx);
    LOG_PRINT(F(", late us="));
    LOG_PRINT(static_cast<unsigned long>(
      uint64_t(uint32_t(LMIC.radio.rxlate_ticks) - rxLateTicksAtTx) * 1000000ULL / OSTICKS_PER_SEC));
    LOG_PRINT(F(", next FCntDn="));
    LOG_PRINTLN(LMIC.seqnoDn);
#endif
    timeFailureReported = true;
  }
  // Do not advance LMIC here. EV_TXCOMPLETE must persist counters and receive
  // state before its engine sends any automatic MAC response.
}

static void startMeasurement()
{
  slm.start();
  measurementStartedUs = time_us_64();
  measurementWindowEndUs = measurementStartedUs + uint64_t(SEND_AFTER) * 1000000ULL;
#if defined(ARDUINO_ARCH_RP2040) && FEATHER_DUAL_CORE
  measurementWindowEndMs = millis() + SEND_AFTER * 1000UL;
#endif
  recording = true;
  if (!measurementHasStarted) {
    LOG_PRINTLN(F("Microphone capture started; five-minute summaries"));
    measurementHasStarted = true;
  }
}

// LMIC jobs to be called from os run loop.
static osjob_t sendjob;
static osjob_t restartjob;

// Keep the LMIC session and queued MAC answers between measurement windows.
void onEvent(ev_t ev)
{
  switch (ev)
  {
    case EV_TXSTART:
    {
      rx1IrqFlags = 0;
      rx2Opened = false;
      rxLateCountAtTx = LMIC.radio.rxlate_count;
      rxLateTicksAtTx = LMIC.radio.rxlate_ticks;
      // Inspect the actual frame: automatic FPort 0 MAC replies must not
      // light the application-payload LED, even if pendTxPort is stale.
      const uint8_t portOffset = OFF_DAT_OPTS + (LMIC.frame[OFF_DAT_FCT] & 0x0F);
      const bool applicationPayload = LMIC.dataLen > portOffset + 5 &&
                                      LMIC.frame[portOffset] == 1;
      digitalWrite(LED_BUILTIN, applicationPayload ? HIGH : LOW);
#if USB_LOGGING && USB_RADIO_LOGGING
      LOG_PRINT(os_getTime());
      LOG_PRINT(F(": "));
      LOG_PRINTLN(F("LoRa transmission started"));
#endif
      break;
    }
    case EV_RXSTART:
      if (LMIC.txrxFlags & TXRX_DNW2) {
        rx1IrqFlags = LMIC.saveIrqFlags;
        rx2Opened = true;
      }
      break;
    case EV_TXCANCELED:
      digitalWrite(LED_BUILTIN, LOW);
      break;
    case EV_TXCOMPLETE:
      digitalWrite(LED_BUILTIN, LOW);
#if USB_LOGGING && USB_RADIO_LOGGING
      LOG_PRINT(os_getTime());
      LOG_PRINT(F(": "));
      LOG_PRINTLN(F("EV_TXCOMPLETE (includes waiting for RX windows)"));
      if (LMIC.txrxFlags & TXRX_ACK)
      {
        LOG_PRINTLN(F("Received ack"));
      }
      if (LMIC.dataLen)
      {
        LOG_PRINTLN(F("Received "));
        LOG_PRINTLN(LMIC.dataLen);
        LOG_PRINTLN(F(" bytes of payload"));
      }
#endif
      // LMIC can schedule a MAC-only uplink immediately after this callback.
      // Commit counters before returning to its engine, with I2S stopped.
      if (!counters.saveDown(LMIC.seqnoDn) || !counters.ensureReserved(LMIC.seqnoUp) ||
          !saveReceiveState())
      {
        failClosed(F("Counter or receive-state save failed; radio stopped"));
        break;
      }
      transmissionComplete = true;
      break;
  }
}

// Pack the same application bytes that the Pico prints and TTN decodes.
// The timestamp belongs to the completed measurement window, never delivery.
void do_send(osjob_t *, const FeatherMeasurementSnapshot *snapshot = nullptr)
{
  if (!radioReady)
  {
    LOG_PRINTLN(F("Measurement publication dropped; radio session is rebuilding"));
    return;
  }
  if (LMIC.opmode & (OP_TXRXPEND | OP_TXDATA | OP_POLL))
  {
    LOG_PRINTLN(F("Measurement publication dropped; radio is busy"));
    return;
  }
  if (!sessionKeysMatch())
  {
    failClosed(F("ABP session key check failed; radio stopped"));
    return;
  }
  if (!counters.ensureReserved(LMIC.seqnoUp) || !saveReceiveState())
  {
    failClosed(F("Counter or receive-state reservation failed; radio stopped"));
    return;
  }

  uint32_t windowEndUtc = 0;
  const uint64_t windowEndUs = snapshot ? snapshot->windowEndUs : measurementWindowEndUs;
  const bool haveUtc = utcClock.secondsAt(windowEndUs, windowEndUtc);
  FeatherMeasurementPayload payload;
  const double mean = snapshot ? snapshot->mean : slm.statsDB.Mean();
  const double minimum = snapshot ? snapshot->minimum : slm.statsDB.Min();
  const double maximum = snapshot ? snapshot->maximum : slm.statsDB.Max();
  const double stddev = snapshot ? snapshot->stddev : slm.statsDB.StdDev(true);
  const double leq = snapshot ? snapshot->leq :
      120 + db_full_scale_pow(slm.statsRMS.Mean());
  if (!payload.encode(mean, minimum, maximum, stddev, leq, windowEndUtc, haveUtc))
  {
    failClosed(F("Measurement timestamp could not be encoded; radio stopped"));
    return;
  }
#if FEATHER_BATTERY_ENABLED
  if (!payload.appendBattery(static_cast<uint16_t>(analogRead(FEATHER_BATTERY_ADC_PIN))))
  {
    failClosed(F("Battery ADC reading could not be encoded; radio stopped"));
    return;
  }
#endif

  // Piggyback DeviceTimeReq on an ordinary uplink. Acoustic summaries keep
  // flowing without a UTC field when a response has not arrived.
  if (utcClock.needsSynchronization(time_us_64(), FEATHER_CLOCK_RESYNC_SECONDS))
  {
    LMIC_requestNetworkTime(networkTimeReceived, nullptr);
  }

  if (LMIC_setTxData2(1, payload.bytes, payload.length, 0) != 0)
  {
    LOG_PRINTLN(F("Measurement publication dropped; radio rejected the packet"));
    return;
  }
#if USB_LOGGING
  LOG_PRINT(F("PAYLOAD_HEX="));
  for (size_t i = 0; i < payload.length; ++i) {
    if (payload.bytes[i] < 0x10) LOG_PRINT('0');
    LOG_PRINT(payload.bytes[i], HEX);
  }
  LOG_PRINTLN();
#endif
  os_setTimedCallback(&restartjob, os_getTime()+sec2osticks(120), restart);
}

// A radio timeout drops the current publication and rebuilds the ABP session.
void restart(osjob_t *j)
{
  LOG_PRINTLN(F("LoRa transmission timed out; dropping publication and rebuilding the session"));
  os_clearCallback(&restartjob);
  LMIC_shutdown();
  radioReady = false;
  transmissionComplete = false;
  transportRecoveryRequested = true;
}

// Setup
void setup()
{
#if USB_LOGGING
  Serial.begin(115200);
  // Do not stall deployments if no USB host/Serial Monitor is connected.
  uint32_t serial_wait_started = millis();
  while (!Serial && (millis() - serial_wait_started < 5000))
  {
  }
  LOG_PRINTLN(F("Feather TTN " FEATHER_FIRMWARE_VERSION
                " - fixed five-minute scheduler, flash-safe dual-core audio"));
#endif
#if defined(ARDUINO_ARCH_RP2040)
  // Reset only while the audio core is still making progress. A stuck core
  // then produces an automatic reboot instead of an indefinitely silent node.
  rp2040.wdt_begin(8000);
#endif

  // Keep the RFM95 deselected while the microphone is recording. The radio
  // chip-select pin differs between the Feather M0 and Feather RP2040.
  pinMode(LORA_CS_PIN, OUTPUT);
  digitalWrite(LORA_CS_PIN, HIGH);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
#if FEATHER_BATTERY_ENABLED
  analogReadResolution(12);
  pinMode(FEATHER_BATTERY_ADC_PIN, INPUT);
#endif

  // Initialize the radio and its persistent counters while I2S is stopped.
  if (!slm.setup())
  {
    LOG_PRINTLN(F("Audio FFT initialization failed; device stopped"));
    halted = true;
    return;
  }
  connect();
#if defined(ARDUINO_ARCH_RP2040) && FEATHER_DUAL_CORE
  if (!halted) audioBootstrapReady = true;
#else
  if (!halted) startMeasurement();
#endif
}

#if defined(ARDUINO_ARCH_RP2040) && FEATHER_DUAL_CORE
// Arduino-Pico runs setup1/loop1 on RP2040 core 1. Audio capture and DSP stay
// there, leaving core 0 free for LMIC timing and transport.
void setup1() {}

void loop1()
{
  audioHeartbeatMs = millis();
  if (!audioBootstrapReady || halted) {
    delay(1);
    return;
  }

  if (audioPauseRequest) {
    const uint32_t request = audioPauseRequest;
    if (recording) {
      slm.stop();
      recording = false;
    }
    audioPaused = true;
    audioPauseAck = request;
    while (audioPauseRequest == request && !halted) {
      audioHeartbeatMs = millis();
      delay(1);
    }
    if (!halted && !audioPauseRequest) {
      slm.start();
      recording = true;
      audioPaused = false;
    }
    return;
  }
  if (!recording) {
    startMeasurement();
    return;
  }

  slm.update();
  if (static_cast<int32_t>(millis() - measurementWindowEndMs) >= 0) {
    const uint64_t completedWindowEndUs = time_us_64();
    if (!measurementSnapshotReady) {
      measurementSnapshot.mean = slm.statsDB.Mean();
      measurementSnapshot.minimum = slm.statsDB.Min();
      measurementSnapshot.maximum = slm.statsDB.Max();
      measurementSnapshot.stddev = slm.statsDB.StdDev(true);
      measurementSnapshot.leq = 120 + db_full_scale_pow(slm.statsRMS.Mean());
      measurementSnapshot.windowEndUs = completedWindowEndUs;
      measurementSnapshotReady = true;
    }

    // Advance from the scheduled boundary, rather than from the actual radio
    // completion time. If a snapshot is dropped, the next window remains on
    // the same five-minute cadence.
    measurementStartedUs = completedWindowEndUs;
    measurementWindowEndMs += FEATHER_MEASUREMENT_SECONDS * 1000UL;
    // Keep the PIO/DMA I2S stream running across boundaries. Reinitializing
    // I2S on core 1 after every window can block on a peripheral that is
    // already active. Reset only the acoustic aggregates; the next window
    // therefore starts immediately without a capture gap.
    slm.reset();
  }
}
#endif

// Start up the LoRa transmission and put the send job in the LoRa os loop
void connect()
{
  if (radioReady) return;
#if USB_LOGGING
  LOG_PRINTLN(F("Starting LoRaWAN ABP session for TTN (no OTAA join)"));
#endif
  // LMIC init or use os_init with global pinmap;
  os_init_ex(pPinMap);

  // Reset the MAC state. Session and pending data transfers will be discarded.
  LMIC_reset();

  // Next code only needed for ABP
#if ABP_MODE == 1
  uint8_t appskey[sizeof(APPSKEY)];
  uint8_t nwkskey[sizeof(NWKSKEY)];
  memcpy_P(appskey, APPSKEY, sizeof(APPSKEY));
  memcpy_P(nwkskey, NWKSKEY, sizeof(NWKSKEY));
  uint8_t anyApp = 0, anyNwk = 0;
  for (uint8_t i = 0; i < 16; ++i) { anyApp |= appskey[i]; anyNwk |= nwkskey[i]; }
  if (!DEVADDR || !anyApp || !anyNwk || memcmp(appskey, nwkskey, 16) == 0)
  {
    failClosed(F("Feather credentials are missing or invalid; radio stopped"));
    return;
  }
  // Bind the stored counters to this exact address and pair of session keys.
  uint64_t identityTag = 14695981039346656037ULL;
  for (uint8_t i = 0; i < 4; ++i)
    identityTag = (identityTag ^ ((DEVADDR >> (i * 8)) & 0xFF)) * 1099511628211ULL;
  for (uint8_t i = 0; i < 16; ++i)
    identityTag = (identityTag ^ nwkskey[i]) * 1099511628211ULL;
  for (uint8_t i = 0; i < 16; ++i)
    identityTag = (identityTag ^ appskey[i]) * 1099511628211ULL;
  sessionIdentityTag = identityTag;
  if (!startCounterStore(identityTag))
  {
    failClosed(F("Counter storage unavailable: use 64KB filesystem; preserve existing state"));
    return;
  }
  LMIC_setSession(FEATHER_NET_ID, DEVADDR, nwkskey, appskey);
  if (!sessionKeysMatch())
  {
    failClosed(F("ABP session keys were not loaded correctly; radio stopped"));
    return;
  }
  LOG_PRINTLN(F("ABP session keys loaded and verified in LMIC"));
  LOG_PRINTLN(F("ABP identity loaded; device address and EUI are intentionally not printed"));
  LMIC.seqnoUp = counters.startUp();
  LMIC.seqnoDn = counters.startDown();
  LMIC_setupChannel(0, 868100000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
  LMIC_setupChannel(1, 868300000, DR_RANGE_MAP(DR_SF12, DR_SF7B), BAND_CENTI);
  LMIC_setupChannel(2, 868500000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
  for (uint8_t i = 3; i < 8; ++i)
    LMIC_setupChannel(i, 867100000 + (i - 3) * 200000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
  LMIC_setAdrMode(0);
  LMIC_setLinkCheckMode(0);
  LMIC_setDrTxpow(DR_SF7, 14);
  if (!startReceiveStore())
  {
    failClosed(F("Receive-state storage is invalid; radio stopped"));
    return;
  }
  radioReady = true;
#if USB_LOGGING
  LOG_PRINT(F("Separate Feather ABP session ready; next FCntUp="));
  LOG_PRINTLN(LMIC.seqnoUp);
  LOG_PRINT(F("Receive settings: RX1="));
  LOG_PRINT(LMIC.rxDelay);
  LOG_PRINT(F("s, offset="));
  LOG_PRINT(LMIC.rx1DrOffset);
  LOG_PRINT(F("; RX2=DR"));
  LOG_PRINT(LMIC.dn2Dr);
  LOG_PRINT('@');
  LOG_PRINT(LMIC.dn2Freq);
  LOG_PRINT(F("; next FCntDn="));
  LOG_PRINTLN(LMIC.seqnoDn);
#endif
#else
#if USB_LOGGING
  LOG_PRINTLN(F("Currently only ABP_MODE supported"));
#endif
#endif

}

void loop()
{
#if USB_LOGGING
  eventLog.service();
#endif
#if defined(ARDUINO_ARCH_RP2040)
  if (!FEATHER_DUAL_CORE || static_cast<uint32_t>(millis() - audioHeartbeatMs) < 3000)
    rp2040.wdt_reset();
#endif
  if (halted) { delay(10); return; }
  if (transportRecoveryRequested) {
    transportRecoveryRequested = false;
    connect();
    if (halted) return;
  }
  if (transmissionComplete && !(LMIC.opmode & (OP_TXRXPEND | OP_TXDATA | OP_POLL)))
  {
    transmissionComplete = false;
    os_clearCallback(&restartjob);
    if (!counters.saveDown(LMIC.seqnoDn))
    {
      failClosed(F("Downlink counter save failed; transmission stopped"));
      return;
    }
#if !defined(ARDUINO_ARCH_RP2040) || !FEATHER_DUAL_CORE
    startMeasurement();
#endif
  }
#if defined(ARDUINO_ARCH_RP2040) && FEATHER_DUAL_CORE
  if (measurementSnapshotReady && !halted) {
    FeatherMeasurementSnapshot snapshot = measurementSnapshot;
    measurementSnapshotReady = false;
    do_send(&sendjob, &snapshot);
    if (halted) return;
  }
#endif
#if !defined(ARDUINO_ARCH_RP2040) || !FEATHER_DUAL_CORE
  if (recording && time_us_64() >= measurementWindowEndUs)
  {
    slm.stop();
    recording = false;
    do_send(&sendjob);
    if (halted) return;
  }
#endif
  // Call update as often as possible while recording, else call the runloop of LMIC
#if !defined(ARDUINO_ARCH_RP2040) || !FEATHER_DUAL_CORE
  if (recording)
  {
    slm.update();
  }
#endif
#if !defined(ARDUINO_ARCH_RP2040) || !FEATHER_DUAL_CORE
  else
  {
    os_runloop_once();
  }
#else
  os_runloop_once();
#endif
}
