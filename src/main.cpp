/*
SRFM 4-channel pneumatic valve controller -- custom PCB firmware.

  Seeed XIAO nRF52840  --SPI-->  AD5724R quad 12-bit DAC  --0..10 V-->  4 x SMC ITV valves
                       --I2C-->  ADS1015 ADC  <--1..5 V monitor--  (valve readback)
                       --GPIO->  TPS26600 eFuse SHDN / FLT      (24 V valve rail)

Hardware facts, register sequences and safety rules: FIRMWARE_HANDOFF.md.
Host command set: PROTOCOL.md. Both are authoritative; this file follows them.

The board's pull resistors define the safe state (DAC cleared, rail off)
whenever the MCU's pins are high-Z: before boot, in reset, after a watchdog
reset. This firmware only ever *leaves* that state deliberately, in the
order of FIRMWARE_HANDOFF section 6, and returns to it on STOP, on an eFuse
fault, on host-link loss, and -- via the watchdog -- on a hang.

Channels are logical CH1..CH4 (index 0..3 internally). Which physical DAC
output and ADC input serve a channel is data in ValveConfig, set with
CAL MAP from the bench measurement, never assumed from the schematic.
*/

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

#include "AD5724R.h"
#include "ADS1015.h"
#include "ValveConfig.h"
#include "PressureControl.h"
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

static const char *FW_ID = "SRFM-PCB v2.0";

// --- pins (FIRMWARE_HANDOFF section 1, as built) ------------------------
static const uint8_t PIN_DAC_SYNC   = D2;   // P0.28, 10 k pull-up
static const uint8_t PIN_DAC_CLR    = D3;   // P0.29, 10 k pull-down: low = cleared, writes ignored
static const uint8_t PIN_EFUSE_SHDN = D6;   // P1.11, 10 k pull-down: high = 24 V rail on
static const uint8_t PIN_EFUSE_FLT  = D7;   // P1.12, 100 k pull-up, open-drain, low = fault latched
// SPI SCK/MISO/MOSI = D8/D9/D10 and I2C SDA/SCL = D4/D5 are the core defaults.

// --- timing ------------------------------------------------------------
static const uint32_t WDT_TIMEOUT_MS      = 2000;
static const uint32_t HOUSEKEEP_PERIOD_MS = 100;   // ~10 Hz monitor sweep
static const uint8_t  ADC_AVERAGE         = 8;     // per channel per sweep
static const uint32_t RAIL_SETTLE_MS      = 150;   // eFuse ramps ~90 ms
static const uint32_t STUCK_HOLD_MS       = 1000;
static const uint32_t CLEARFAULT_OFF_MS   = 120;   // SHDN low >= 100 ms to clear a latch

// --- monitor thresholds (FIRMWARE_HANDOFF section 7) --------------------
static const int16_t ADC_CODE_OPEN   = 157;    // < 0.5 V monitor: open load / no rail
static const int16_t ADC_CODE_HIGH   = 1750;   // > 5.6 V monitor: wiring fault
static const int16_t ADC_CODE_NORAIL = 100;    // all channels below ~0.3 V: rail is off
static const float   STUCK_TOLERANCE = 0.10f;  // fraction of full scale

// --- state -------------------------------------------------------------
enum class BoardState { BOOT, READY, STOPPED, FAULT };
enum class FaultReason { NONE, SPI, FLT, LINKLOST };
enum class ChStatus { OK, OPEN, HIGH_, STUCK, OC, NA, NORAIL };

struct Channel
{
  float    cmdFrac   = 0.0f;   // commanded fraction of full scale (0..1)
  uint16_t code      = 0;      // DAC code last written for this channel
  int16_t  adcCode   = 0;      // last averaged monitor conversion
  float    monVolts  = 0.0f;
  float    monFrac   = 0.0f;   // monitor as fraction of full scale
  bool     monValid  = false;  // a conversion succeeded this sweep
  ChStatus status    = ChStatus::NA;
  uint32_t stepAtMs  = 0;      // last command change (for stuck detection)
  uint32_t mismatchSinceMs = 0;
  bool     stuckReported = false;
  bool     ocReported = false;
};

static AD5724R          dac;
static ADS1015          adc;
static ValveConfigStore store;
static Channel          chan[VALVE_NUM_CHANNELS];

static BoardState  state = BoardState::BOOT;
static FaultReason faultReason = FaultReason::NONE;
static bool     dacOk = false;          // readback proof passed
static bool     adcOk = false;
static bool     railCmd = false;        // SHDN driven high
static uint32_t railOnAtMs = 0;
static bool     fltLatched = false;
static uint16_t dacPowerReadback = 0;
static bool     tsdReported = false;
static bool     norailReported = false;

static uint32_t lastCmdMs = 0;
static bool     hbArmed = false;        // arms on the first command after boot/START
static uint32_t lastHousekeepMs = 0;

static char    lineBuf[128];
static uint8_t lineLen = 0;
static char    reply[400];   // CAL GET is the longest line (~280 chars)
static uint32_t resetReason = 0;   // NRF_POWER->RESETREAS as seen at boot (diagnostic, shown by ID)
static uint32_t bootWord = 0;      // raw .noinit request word as seen at boot (diagnostic, shown by ID)
static const uint32_t DFU_REQUEST_MAGIC = 0x44465521UL;   // "DFU!"
static const uint32_t RAM_TEST_MAGIC    = 0x54455354UL;   // "TEST" (RAMTEST diagnostic only)
static const char    *DFU_REQUEST_PATH  = "/dfu_req";     // flash-backed copy of the request
static uint32_t dfuRequest __attribute__((section(".noinit")));

// --- small helpers -----------------------------------------------------
static const char *stateName(BoardState s)
{
  switch (s)
  {
    case BoardState::READY:   return "READY";
    case BoardState::STOPPED: return "STOPPED";
    case BoardState::FAULT:   return "FAULT";
    default:                  return "BOOT";
  }
}

static const char *faultName(FaultReason r)
{
  switch (r)
  {
    case FaultReason::SPI:      return "SPI";
    case FaultReason::FLT:      return "FLT";
    case FaultReason::LINKLOST: return "LINKLOST";
    default:                    return "NONE";
  }
}

static const char *chStatusName(ChStatus s)
{
  switch (s)
  {
    case ChStatus::OK:     return "ok";
    case ChStatus::OPEN:   return "open";
    case ChStatus::HIGH_:  return "high";
    case ChStatus::STUCK:  return "stuck";
    case ChStatus::OC:     return "oc";
    case ChStatus::NORAIL: return "norail";
    default:               return "n/a";
  }
}

static inline ChannelCal &cal(uint8_t i) { return store.cfg().ch[i]; }
static inline bool railOn() { return railCmd && !fltLatched; }

static void printfln(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(reply, sizeof(reply), fmt, ap);
  va_end(ap);
  Serial.println(reply);
}

// --- watchdog (nRF52840 WDT, 32.768 kHz) -------------------------------
// A WDT reset returns every GPIO to high-Z: CLR asserted, SHDN low. Kicked
// only from loop() after a housekeeping pass, never from an ISR.
static void wdtStart()
{
  if (NRF_WDT->RUNSTATUS & WDT_RUNSTATUS_RUNSTATUS_Msk) return;   // survives soft reset
  NRF_WDT->CONFIG = (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos) |
                    (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos);
  NRF_WDT->CRV = (uint32_t)(WDT_TIMEOUT_MS * 32768UL / 1000UL);
  NRF_WDT->RREN = WDT_RREN_RR0_Msk;
  NRF_WDT->TASKS_START = 1;
}

static inline void wdtKick() { NRF_WDT->RR[0] = WDT_RR_RR_Reload; }

extern "C" void __wrap_enterSerialDfu(void);   // defined below, before setup()

// --- DAC / valve command path ------------------------------------------
static void applyCode(uint8_t i, uint16_t code)
{
  Channel &c = chan[i];
  const ChannelCal &k = cal(i);
  if (code > k.codeFullScale) code = k.codeFullScale;   // hard ceiling: never over-drive the valve
  dac.writeCode(k.dacAddr, code);
  if (code != c.code)
  {
    c.stepAtMs = millis();
    c.mismatchSinceMs = 0;
    c.stuckReported = false;
  }
  c.code = code;
  c.cmdFrac = PressureControl::codeToFraction(k, code);
}

static void applyFraction(uint8_t i, float frac)
{
  applyCode(i, PressureControl::fractionToCode(cal(i), frac));
}

static void zeroAllOutputs()
{
  for (uint8_t i = 0; i < VALVE_NUM_CHANNELS; i++) applyCode(i, 0);
}

// --- rail ---------------------------------------------------------------
static void railSet(bool on)
{
  digitalWrite(PIN_EFUSE_SHDN, on ? HIGH : LOW);
  railCmd = on;
  if (on)
  {
    railOnAtMs = millis();
    norailReported = false;
    for (uint8_t i = 0; i < VALVE_NUM_CHANNELS; i++)
    {
      chan[i].mismatchSinceMs = 0;
      chan[i].stuckReported = false;
    }
  }
}

//! Shutdown sequence (STOP, e-stop, any fault): command to zero *before*
//! removing the rail so the valves vent instead of retaining pressure.
static void shutdown()
{
  zeroAllOutputs();
  delay(50);
  railSet(false);
}

// --- boot sequence (FIRMWARE_HANDOFF section 6) ------------------------
//! Steps 4-9: leave the DAC safe state and prove the SPI link. Returns false
//! (and leaves the rail off) if the readback does not show power-up.
static bool dacBringUp()
{
  dac.clearRelease();                                    // 4: until now every write was ignored
  dac.writePowerControl(AD5724R_PU_ALL);                 // 5: reference + all four channels
  delayMicroseconds(20);
  dac.writeRange(AD5724R_ADDR_ALL, AD5724R_RANGE_10V8);  // 6: before any code
  dac.writeControlFunction(AD5724R_FUNC_CLAMP_ENABLE | AD5724R_FUNC_TSD_ENABLE);  // 7
  for (uint8_t a = 0; a < AD5724R_NUM_OUTPUTS; a++) dac.writeCode(a, 0);          // 8
  for (uint8_t i = 0; i < VALVE_NUM_CHANNELS; i++) { chan[i].code = 0; chan[i].cmdFrac = 0.0f; }
  dacOk = dac.verifyPowerUp(&dacPowerReadback);          // 9
  return dacOk;
}

static void readMonitors();   // fwd
static void evaluateChannels();

//! Steps 11-12: rail on, settle, first monitor sweep. Emits !READY / !FAULT.
static bool railBringUp()
{
  fltLatched = false;
  railSet(true);                                         // 11 (no-op if MainPower is OFF)
  delay(RAIL_SETTLE_MS);                                 // 12
  fltLatched = (digitalRead(PIN_EFUSE_FLT) == LOW);
  if (fltLatched)
  {
    shutdown();
    state = BoardState::FAULT;
    faultReason = FaultReason::FLT;
    printfln("!FAULT FLT");
    return false;
  }
  readMonitors();
  evaluateChannels();
  state = BoardState::READY;
  faultReason = FaultReason::NONE;
  hbArmed = false;
  printfln("!READY ch1=%s ch2=%s ch3=%s ch4=%s",
           chStatusName(chan[0].status), chStatusName(chan[1].status),
           chStatusName(chan[2].status), chStatusName(chan[3].status));
  return true;
}

//! START: re-run steps 8-12.
static bool restart()
{
  if (!dacOk)
  {
    if (!dacBringUp())
    {
      state = BoardState::FAULT;
      faultReason = FaultReason::SPI;
      return false;
    }
  }
  else
  {
    zeroAllOutputs();                                    // 8
    dacOk = dac.verifyPowerUp(&dacPowerReadback);        // 9
    if (!dacOk) { state = BoardState::FAULT; faultReason = FaultReason::SPI; return false; }
  }
  return railBringUp();
}

// --- monitors -------------------------------------------------------------
static void readMonitors()
{
  for (uint8_t i = 0; i < VALVE_NUM_CHANNELS; i++)
  {
    Channel &c = chan[i];
    const ChannelCal &k = cal(i);
    c.monValid = false;
    if (!k.rbEnabled || !adcOk) continue;
    int16_t code;
    if (!adc.readAveraged(k.adcInput, ADC_AVERAGE, &code)) continue;
    c.adcCode = code;
    c.monVolts = PressureControl::adcCodeToMonitorVolts(k, code);
    c.monFrac = PressureControl::monitorVoltsToFraction(c.monVolts);
    c.monValid = true;
  }
}

static void evaluateChannels()
{
  uint32_t now = millis();
  bool railSettled = railOn() && (uint32_t)(now - railOnAtMs) >= RAIL_SETTLE_MS;

  // Rail-alive inference: every enabled monitor below ~0.3 V with SHDN high.
  uint8_t enabled = 0, dead = 0;
  for (uint8_t i = 0; i < VALVE_NUM_CHANNELS; i++)
  {
    if (!chan[i].monValid) continue;
    enabled++;
    if (chan[i].adcCode < ADC_CODE_NORAIL) dead++;
  }
  bool noRail = railSettled && enabled > 0 && dead == enabled;

  static const uint16_t OC_BIT[AD5724R_NUM_OUTPUTS] = { AD5724R_OC_A, AD5724R_OC_B, AD5724R_OC_C, AD5724R_OC_D };

  for (uint8_t i = 0; i < VALVE_NUM_CHANNELS; i++)
  {
    Channel &c = chan[i];
    const ChannelCal &k = cal(i);
    ChStatus s;

    if (dacOk && (dacPowerReadback & OC_BIT[k.dacAddr]))
    {
      s = ChStatus::OC;
      if (!c.ocReported) { printfln("!FAULT OC %u", i + 1); c.ocReported = true; }
    }
    else
    {
      c.ocReported = false;
      if (!c.monValid)             s = ChStatus::NA;
      else if (!railSettled)       s = railOn() ? ChStatus::OK : ChStatus::NORAIL;
      else if (noRail)             s = ChStatus::NORAIL;
      else if (c.adcCode < ADC_CODE_OPEN) s = ChStatus::OPEN;
      else if (c.adcCode > ADC_CODE_HIGH) s = ChStatus::HIGH_;
      else
      {
        // Stuck valve / lost supply pressure: monitor disagrees with the
        // command by more than 10 % F.S. continuously for longer than 1 s,
        // counted from the later of the last step and the rail coming up
        // (the valve and the pneumatics need that long to settle).
        bool mismatch = fabsf(c.monFrac - c.cmdFrac) > STUCK_TOLERANCE;
        if (!mismatch) c.mismatchSinceMs = 0;
        else if (c.mismatchSinceMs == 0) c.mismatchSinceMs = now;
        uint32_t since = max(max(c.stepAtMs, railOnAtMs), c.mismatchSinceMs);
        s = (mismatch && (uint32_t)(now - since) > STUCK_HOLD_MS) ? ChStatus::STUCK : ChStatus::OK;
      }
    }

    if (s == ChStatus::OPEN && c.status != ChStatus::OPEN) printfln("!FAULT OPENLOAD %u", i + 1);
    if (s == ChStatus::STUCK && !c.stuckReported) { printfln("!FAULT STUCK %u", i + 1); c.stuckReported = true; }
    if (s != ChStatus::STUCK) c.stuckReported = false;
    c.status = s;
  }

  if (noRail && !norailReported) { printfln("!FAULT NORAIL"); norailReported = true; }
  if (!noRail) norailReported = false;
}

// --- housekeeping: monitors, DAC status, FLT, heartbeat -------------------
static void housekeeping()
{
  uint32_t now = millis();

  if (dacOk)
  {
    dacPowerReadback = dac.readPowerControl();
    if (dacPowerReadback & AD5724R_TSD)
    {
      if (!tsdReported) { printfln("!FAULT TSD"); tsdReported = true; }
    }
    else tsdReported = false;
  }

  // eFuse latch: only meaningful once the rail has been commanded on.
  if (railCmd && (uint32_t)(now - railOnAtMs) >= RAIL_SETTLE_MS && digitalRead(PIN_EFUSE_FLT) == LOW)
  {
    if (!fltLatched)
    {
      fltLatched = true;
      shutdown();
      state = BoardState::FAULT;
      faultReason = FaultReason::FLT;
      hbArmed = false;
      printfln("!FAULT FLT");
    }
  }

  readMonitors();
  evaluateChannels();

  // Host link loss.
  uint16_t hb = store.cfg().hbTimeout_s;
  if (state == BoardState::READY && hb > 0 && hbArmed &&
      (uint32_t)(now - lastCmdMs) > (uint32_t)hb * 1000UL)
  {
    shutdown();
    state = BoardState::FAULT;
    faultReason = FaultReason::LINKLOST;
    hbArmed = false;
    printfln("!FAULT LINKLOST");
  }
}

// --- command parsing ------------------------------------------------------
static bool parseChannel(const char *s, uint8_t *idx)
{
  if (s == nullptr) return false;
  char *end;
  long v = strtol(s, &end, 10);
  if (*end != '\0' || v < 1 || v > VALVE_NUM_CHANNELS) return false;
  *idx = (uint8_t)(v - 1);
  return true;
}

static bool parseFloat(const char *s, float *out)
{
  if (s == nullptr) return false;
  char *end;
  float v = strtof(s, &end);
  if (end == s || *end != '\0' || isnan(v) || isinf(v)) return false;
  *out = v;
  return true;
}

static bool parseLong(const char *s, long *out)
{
  if (s == nullptr) return false;
  char *end;
  long v = strtol(s, &end, 10);
  if (end == s || *end != '\0') return false;
  *out = v;
  return true;
}

static bool parseDacAddr(const char *s, uint8_t *addr)
{
  if (s == nullptr || s[1] != '\0') return false;
  char c = (char)toupper((unsigned char)s[0]);
  if (c >= 'A' && c <= 'D') { *addr = (uint8_t)(c - 'A'); return true; }
  if (c >= '0' && c <= '3') { *addr = (uint8_t)(c - '0'); return true; }
  return false;
}

static bool requireReady()
{
  if (state == BoardState::READY) return true;
  if (state == BoardState::FAULT) printfln("ERR board in FAULT %s (CLEARFAULT or START)", faultName(faultReason));
  else printfln("ERR board %s (send START)", stateName(state));
  return false;
}

static void replyMapString(char *out, size_t n)
{
  snprintf(out, n, "%c%c%c%c/%u%u%u%u",
           'A' + cal(0).dacAddr, 'A' + cal(1).dacAddr, 'A' + cal(2).dacAddr, 'A' + cal(3).dacAddr,
           cal(0).adcInput, cal(1).adcInput, cal(2).adcInput, cal(3).adcInput);
}

static void replyHb(char *out, size_t n)
{
  uint16_t hb = store.cfg().hbTimeout_s;
  if (hb == 0) snprintf(out, n, "off");
  else snprintf(out, n, "%u", hb);
}

static void monField(uint8_t i, char *out, size_t n)
{
  // "<pct>%,<volts>V" or "n/a,n/a": monitor as fraction of full scale and as
  // the valve's monitor-pin voltage (1 V = 0 %, 5 V = 100 %).
  const Channel &c = chan[i];
  if (!c.monValid) snprintf(out, n, "n/a,n/a");
  else snprintf(out, n, "%.1f%%,%.3fV", c.monFrac * 100.0f, c.monVolts);
}

static void monPct(uint8_t i, char *out, size_t n)
{
  const Channel &c = chan[i];
  if (!c.monValid) snprintf(out, n, "n/a");
  else snprintf(out, n, "%.1f", c.monFrac * 100.0f);
}

static void cmdId()
{
  char map[16], hb[8];
  replyMapString(map, sizeof(map));
  replyHb(hb, sizeof(hb));
  printfln("OK %s state=%s map=%s cal=%s hb=%s rst=0x%lX bootword=0x%lX", FW_ID, stateName(state), map,
           store.calibrated() ? "ok" : "uncal", hb, (unsigned long)resetReason, (unsigned long)bootWord);
}

static void cmdGetAll()
{
  char m[4][20];
  for (uint8_t i = 0; i < VALVE_NUM_CHANNELS; i++) monField(i, m[i], sizeof(m[i]));
  size_t pos = 0;
  pos += snprintf(reply + pos, sizeof(reply) - pos, "OK ");
  for (uint8_t i = 0; i < VALVE_NUM_CHANNELS; i++)
  {
    const ChannelCal &k = cal(i);
    float v = PressureControl::codeToVolts(k, chan[i].code);
    pos += snprintf(reply + pos, sizeof(reply) - pos, "%s=%.2f,%.3fV,%s%s",
                    ValveConfigStore::channelName(i), PressureControl::voltsToPressure(k, v), v, m[i],
                    i < VALVE_NUM_CHANNELS - 1 ? "; " : "");
    if (pos >= sizeof(reply)) break;
  }
  Serial.println(reply);
}

static void cmdGetOne(uint8_t i)
{
  char m[12];
  monPct(i, m, sizeof(m));
  if (chan[i].monValid)
    printfln("OK ch%u cmd=%.1f mon=%s monv=%.3f status=%s", i + 1, chan[i].cmdFrac * 100.0f, m, chan[i].monVolts, chStatusName(chan[i].status));
  else
    printfln("OK ch%u cmd=%.1f mon=n/a monv=n/a status=%s", i + 1, chan[i].cmdFrac * 100.0f, chStatusName(chan[i].status));
}

static void cmdStatus()
{
  char hb[12];
  replyHb(hb, sizeof(hb));
  size_t pos = 0;
  pos += snprintf(reply + pos, sizeof(reply) - pos, "OK state=%s rail=%s flt=%u dac=0x%04X cal=%s hb=%s",
                  stateName(state), railOn() ? "on" : "off", fltLatched ? 1 : 0, dacPowerReadback,
                  store.calibrated() ? "ok" : "uncal", hb);
  if (state == BoardState::FAULT)
    pos += snprintf(reply + pos, sizeof(reply) - pos, " reason=%s", faultName(faultReason));
  for (uint8_t i = 0; i < VALVE_NUM_CHANNELS; i++)
  {
    char m[12];
    monPct(i, m, sizeof(m));
    pos += snprintf(reply + pos, sizeof(reply) - pos, " ch%u=%.1f,%s,%s",
                    i + 1, chan[i].cmdFrac * 100.0f, m, chStatusName(chan[i].status));
    if (pos >= sizeof(reply)) break;
  }
  Serial.println(reply);
}

static void cmdDump()
{
  size_t pos = 0;
  pos += snprintf(reply + pos, sizeof(reply) - pos, "OK ");
  for (uint8_t i = 0; i < VALVE_NUM_CHANNELS; i++)
  {
    const ChannelCal &k = cal(i);
    pos += snprintf(reply + pos, sizeof(reply) - pos, "ch%u=%u,dac%c,ain%u,%d,%.3fV%s",
                    i + 1, chan[i].code, 'A' + k.dacAddr, k.adcInput, chan[i].adcCode, chan[i].monVolts,
                    i < VALVE_NUM_CHANNELS - 1 ? "; " : "");
    if (pos >= sizeof(reply)) break;
  }
  Serial.println(reply);
}

static void cmdCalGet()
{
  size_t pos = 0;
  pos += snprintf(reply + pos, sizeof(reply) - pos, "OK cal=%s", store.calibrated() ? "ok" : "uncal");
  for (uint8_t i = 0; i < VALVE_NUM_CHANNELS; i++)
  {
    const ChannelCal &k = cal(i);
    pos += snprintf(reply + pos, sizeof(reply) - pos,
                    " ch%u=fs:%u,rb:%.3f/%d,rben:%u,dac:%c,adc:%u,p:%.2f/%.2f/%.2f/%.2f",
                    i + 1, k.codeFullScale, k.rbGain_mV, k.rbOffset, k.rbEnabled, 'A' + k.dacAddr, k.adcInput,
                    k.vMin, k.vMax, k.pMin, k.pMax);
    if (pos >= sizeof(reply)) break;
  }
  Serial.println(reply);
}

static bool setPressureCal(uint8_t i, const char *a, const char *b, const char *c, const char *d)
{
  float vMin, vMax, pMin, pMax;
  if (!parseFloat(a, &vMin) || !parseFloat(b, &vMax) || !parseFloat(c, &pMin) || !parseFloat(d, &pMax)) return false;
  if (vMin == vMax || pMin == pMax) return false;
  if (vMin < 0.0f || vMin > VALVE_VOLTS_MAX || vMax < 0.0f || vMax > VALVE_VOLTS_MAX) return false;
  ChannelCal &k = cal(i);
  k.vMin = vMin; k.vMax = vMax; k.pMin = pMin; k.pMax = pMax;
  return true;
}

static void cmdCal(char *sub)
{
  if (sub == nullptr) { printfln("ERR usage: CAL GET|SAVE|DEFAULT|FS|RB|RBEN|MAP|PRESS ..."); return; }
  for (char *p = sub; *p; p++) *p = (char)toupper((unsigned char)*p);

  if (strcmp(sub, "GET") == 0) { cmdCalGet(); return; }
  if (strcmp(sub, "SAVE") == 0)
  {
    if (store.save()) printfln("OK saved");
    else printfln("ERR flash write failed");
    return;
  }
  if (strcmp(sub, "DEFAULT") == 0)
  {
    store.setDefaults();
    for (uint8_t i = 0; i < VALVE_NUM_CHANNELS; i++) applyCode(i, min(chan[i].code, cal(i).codeFullScale));
    printfln("OK defaults");
    return;
  }

  // Legacy form: CAL <ch> <vMin> <vMax> <pMin> <pMax>
  uint8_t i;
  if (parseChannel(sub, &i))
  {
    char *a = strtok(nullptr, " \t"), *b = strtok(nullptr, " \t"), *c = strtok(nullptr, " \t"), *d = strtok(nullptr, " \t");
    if (setPressureCal(i, a, b, c, d)) printfln("OK");
    else printfln("ERR usage: CAL <ch> <vMin> <vMax> <pMin> <pMax>");
    return;
  }

  char *chStr = strtok(nullptr, " \t");
  if (!parseChannel(chStr, &i)) { printfln("ERR bad channel (1-%u)", VALVE_NUM_CHANNELS); return; }
  ChannelCal &k = cal(i);

  if (strcmp(sub, "FS") == 0)
  {
    long v;
    if (!parseLong(strtok(nullptr, " \t"), &v) || v < 1 || v > AD5724R_MAX_CODE) { printfln("ERR code 1-4095"); return; }
    k.codeFullScale = (uint16_t)v;
    applyFraction(i, chan[i].cmdFrac);    // keep the commanded fraction under the new scale
    printfln("OK");
  }
  else if (strcmp(sub, "RB") == 0)
  {
    float g; long off;
    if (!parseFloat(strtok(nullptr, " \t"), &g) || !(g > 0.0f) || !parseLong(strtok(nullptr, " \t"), &off) ||
        off < -2048 || off > 2047)
    { printfln("ERR usage: CAL RB <ch> <mV_per_code> <offset_code>"); return; }
    k.rbGain_mV = g;
    k.rbOffset = (int16_t)off;
    printfln("OK");
  }
  else if (strcmp(sub, "RBEN") == 0)
  {
    long v;
    if (!parseLong(strtok(nullptr, " \t"), &v) || (v != 0 && v != 1)) { printfln("ERR usage: CAL RBEN <ch> <0|1>"); return; }
    k.rbEnabled = (uint8_t)v;
    printfln("OK");
  }
  else if (strcmp(sub, "MAP") == 0)
  {
    uint8_t addr; long in;
    if (!parseDacAddr(strtok(nullptr, " \t"), &addr) || !parseLong(strtok(nullptr, " \t"), &in) || in < 0 || in > 3)
    { printfln("ERR usage: CAL MAP <ch> <A-D> <0-3>"); return; }
    // Re-point the channel. If another channel already owns the requested
    // DAC output it takes this channel's old output (a swap), so any
    // permutation can be entered one line at a time. Both outputs are
    // zeroed first so no valve is left holding a command nobody owns.
    uint8_t oldAddr = k.dacAddr;
    dac.writeCode(oldAddr, 0);
    dac.writeCode(addr, 0);
    for (uint8_t j = 0; j < VALVE_NUM_CHANNELS; j++)
      if (j != i && cal(j).dacAddr == addr)
      {
        cal(j).dacAddr = oldAddr;
        applyCode(j, chan[j].code);
      }
    k.dacAddr = addr;
    k.adcInput = (uint8_t)in;
    applyCode(i, chan[i].code);
    printfln("OK");
  }
  else if (strcmp(sub, "PRESS") == 0)
  {
    char *a = strtok(nullptr, " \t"), *b = strtok(nullptr, " \t"), *c = strtok(nullptr, " \t"), *d = strtok(nullptr, " \t");
    if (setPressureCal(i, a, b, c, d)) printfln("OK");
    else printfln("ERR usage: CAL PRESS <ch> <vMin> <vMax> <pMin> <pMax>");
  }
  else printfln("ERR unknown CAL subcommand: %s", sub);
}

static void handleLine(char *line)
{
  char *cmd = strtok(line, " \t");
  if (cmd == nullptr) return;
  for (char *p = cmd; *p; p++) *p = (char)toupper((unsigned char)*p);

  // Any well-formed line counts as a heartbeat (the reply says if it was rejected).
  lastCmdMs = millis();
  hbArmed = true;

  if (strcmp(cmd, "ID") == 0) cmdId();
  else if (strcmp(cmd, "HB") == 0) printfln("OK");
  else if (strcmp(cmd, "HBT") == 0)
  {
    long v;
    if (!parseLong(strtok(nullptr, " \t"), &v) || v < 0 || v > 3600) { printfln("ERR usage: HBT <seconds 0-3600>"); return; }
    store.cfg().hbTimeout_s = (uint16_t)v;
    char hb[12]; replyHb(hb, sizeof(hb));
    printfln("OK hb=%s", hb);
  }
  else if (strcmp(cmd, "SET") == 0)
  {
    uint8_t i; float pct;
    if (!parseChannel(strtok(nullptr, " \t"), &i) || !parseFloat(strtok(nullptr, " \t"), &pct))
    { printfln("ERR usage: SET <ch 1-4> <pct 0-100>"); return; }
    if (pct < 0.0f || pct > 100.0f) { printfln("ERR pct out of range 0-100"); return; }
    if (!requireReady()) return;
    applyFraction(i, pct / 100.0f);
    printfln("OK ch%u pct=%.1f code=%u", i + 1, chan[i].cmdFrac * 100.0f, chan[i].code);
  }
  else if (strcmp(cmd, "SETALL") == 0)
  {
    float pct[VALVE_NUM_CHANNELS];
    for (uint8_t i = 0; i < VALVE_NUM_CHANNELS; i++)
    {
      if (!parseFloat(strtok(nullptr, " \t"), &pct[i])) { printfln("ERR usage: SETALL <p1> <p2> <p3> <p4>"); return; }
      if (pct[i] < 0.0f || pct[i] > 100.0f) { printfln("ERR pct out of range 0-100 (ch%u)", i + 1); return; }
    }
    if (!requireReady()) return;
    for (uint8_t i = 0; i < VALVE_NUM_CHANNELS; i++) applyFraction(i, pct[i] / 100.0f);
    printfln("OK");
  }
  else if (strcmp(cmd, "P") == 0)
  {
    uint8_t i; float p;
    if (!parseChannel(strtok(nullptr, " \t"), &i) || !parseFloat(strtok(nullptr, " \t"), &p))
    { printfln("ERR usage: P <ch 1-4> <pressure>"); return; }
    if (!requireReady()) return;
    const ChannelCal &k = cal(i);
    float applied = PressureControl::clampPressure(k, p);
    float v = PressureControl::pressureToVolts(k, applied);
    applyFraction(i, v / VALVE_VOLTS_MAX);
    printfln("OK %s p=%.2f v=%.3f", ValveConfigStore::channelName(i), applied,
             PressureControl::codeToVolts(k, chan[i].code));
  }
  else if (strcmp(cmd, "V") == 0)
  {
    uint8_t i; float v;
    if (!parseChannel(strtok(nullptr, " \t"), &i) || !parseFloat(strtok(nullptr, " \t"), &v))
    { printfln("ERR usage: V <ch 1-4> <volts 0-10>"); return; }
    if (v < 0.0f || v > VALVE_VOLTS_MAX) { printfln("ERR volts out of range 0-10"); return; }
    if (!requireReady()) return;
    applyFraction(i, v / VALVE_VOLTS_MAX);
    printfln("OK ch%u v=%.3f code=%u", i + 1, PressureControl::codeToVolts(cal(i), chan[i].code), chan[i].code);
  }
  else if (strcmp(cmd, "C") == 0)
  {
    uint8_t i; long code;
    if (!parseChannel(strtok(nullptr, " \t"), &i) || !parseLong(strtok(nullptr, " \t"), &code))
    { printfln("ERR usage: C <ch 1-4> <code>"); return; }
    if (code < 0 || code > cal(i).codeFullScale) { printfln("ERR code out of range 0-%u", cal(i).codeFullScale); return; }
    if (!requireReady()) return;
    applyCode(i, (uint16_t)code);
    printfln("OK ch%u code=%u v=%.3f", i + 1, chan[i].code, PressureControl::codeToVolts(cal(i), chan[i].code));
  }
  else if (strcmp(cmd, "GET") == 0)
  {
    char *a = strtok(nullptr, " \t");
    uint8_t i;
    if (a == nullptr) cmdGetAll();
    else if (parseChannel(a, &i)) cmdGetOne(i);
    else printfln("ERR bad channel (1-%u)", VALVE_NUM_CHANNELS);
  }
  else if (strcmp(cmd, "STATUS") == 0) cmdStatus();
  else if (strcmp(cmd, "ZERO") == 0)
  {
    if (dacOk) zeroAllOutputs();
    printfln("OK all zero");
  }
  else if (strcmp(cmd, "STOP") == 0 || strcmp(cmd, "START") == 0 || strcmp(cmd, "RAIL") == 0)
  {
    // RAIL ON/OFF is the bench spelling of START/STOP: same sequences, same
    // state changes, only the reply wording differs.
    bool isRail = (cmd[0] == 'R');
    bool wantOn;
    if (isRail)
    {
      char *a = strtok(nullptr, " \t");
      if (a) for (char *p = a; *p; p++) *p = (char)toupper((unsigned char)*p);
      if (a && strcmp(a, "FORCE") == 0)
      {
        // Bench only: raise SHDN with no DAC proof and no state change, so
        // the analog supply (AVDD is regulated from the switched 24 V) can
        // be examined while the DAC is not answering. Valves must be off.
        // Cycle SHDN low first: that is how a latched TPS26600 is cleared.
        railSet(false);
        delay(CLEARFAULT_OFF_MS);
        railSet(true);
        delay(RAIL_SETTLE_MS);
        fltLatched = (digitalRead(PIN_EFUSE_FLT) == LOW);
        printfln("OK rail=forced flt=%u", fltLatched ? 1 : 0);
        return;
      }
      if (a == nullptr || (strcmp(a, "ON") != 0 && strcmp(a, "OFF") != 0)) { printfln("ERR usage: RAIL <ON|OFF|FORCE>"); return; }
      wantOn = (strcmp(a, "ON") == 0);
    }
    else wantOn = (cmd[2] == 'A');   // STARt vs STOp

    if (!wantOn)
    {
      if (dacOk) shutdown(); else railSet(false);
      if (state != BoardState::FAULT || faultReason == FaultReason::LINKLOST)
      { state = BoardState::STOPPED; faultReason = FaultReason::NONE; }
      printfln(isRail ? "OK rail=off" : "OK stopped");
      return;
    }
    if (state == BoardState::FAULT && faultReason == FaultReason::FLT) { printfln("FAULT FLT latched (CLEARFAULT)"); return; }
    if (restart()) printfln(isRail ? "OK rail=on" : "OK ready");
    else if (faultReason == FaultReason::SPI) printfln("FAULT SPI readback 0x%04X", dacPowerReadback);
    else printfln("FAULT %s", faultName(faultReason));
  }
  else if (strcmp(cmd, "CLEARFAULT") == 0)
  {
    // The only path that cycles SHDN after an eFuse latch: codes to zero,
    // SHDN low long enough to reset the TPS26600, then the normal bring-up.
    if (dacOk) zeroAllOutputs();
    railSet(false);
    delay(CLEARFAULT_OFF_MS);
    fltLatched = false;
    if (!dacOk && !dacBringUp()) { state = BoardState::FAULT; faultReason = FaultReason::SPI; printfln("FAULT SPI readback 0x%04X", dacPowerReadback); return; }
    if (restart()) printfln("OK ready");
    else printfln("FAULT %s", faultName(faultReason));
  }
  else if (strcmp(cmd, "CAL") == 0) cmdCal(strtok(nullptr, " \t"));
  else if (strcmp(cmd, "DUMP") == 0) cmdDump();
  else if (strcmp(cmd, "VERIFY") == 0)
  {
    uint16_t pc;
    bool ok = dac.verifyPowerUp(&pc);
    dacPowerReadback = pc;
    printfln("OK verify=%s pc=0x%04X", ok ? "yes" : "no", pc);
  }
  else if (strcmp(cmd, "SPIMODE") == 0)
  {
    long m;
    if (!parseLong(strtok(nullptr, " \t"), &m) || (m != 1 && m != 2)) { printfln("ERR usage: SPIMODE <1|2>"); return; }
    dac.setSpiMode(m == 1 ? SPI_MODE1 : SPI_MODE2);
    store.cfg().spiMode = dac.spiMode();
    // Re-run the DAC bring-up so the readback proof is against a known state.
    bool ok = dacBringUp();
    if (!ok && state == BoardState::READY) { shutdown(); state = BoardState::FAULT; faultReason = FaultReason::SPI; }
    printfln("OK spimode=%ld verify=%s pc=0x%04X", m, ok ? "yes" : "no", dacPowerReadback);
  }
  else if (strcmp(cmd, "RAW") == 0)
  {
    uint8_t addr; long code;
    if (!parseDacAddr(strtok(nullptr, " \t"), &addr) || !parseLong(strtok(nullptr, " \t"), &code) || code < 0 || code > AD5724R_MAX_CODE)
    { printfln("ERR usage: RAW <A-D> <code 0-4095>"); return; }
    if (!dac.clearReleased()) { printfln("ERR DAC not brought up (START)"); return; }
    dac.writeCode(addr, (uint16_t)code);
    // Keep the logical channel that owns this output honest about what it holds.
    for (uint8_t i = 0; i < VALVE_NUM_CHANNELS; i++)
      if (cal(i).dacAddr == addr)
      {
        chan[i].code = (uint16_t)code;
        chan[i].cmdFrac = PressureControl::codeToFraction(cal(i), (uint16_t)code);
        chan[i].stepAtMs = millis();
      }
    printfln("OK dac%c code=%ld vdac=%.3f", 'A' + addr, code, AD5724R::codeToDacVolts((uint16_t)code));
  }
  else if (strcmp(cmd, "RAWGET") == 0)
  {
    // Read the DAC's own registers for one output: what code it holds and
    // which range it is in. Proves a RAW write landed even if no voltage
    // shows at the pads (which would then point at AVDD or the output stage).
    uint8_t addr;
    if (!parseDacAddr(strtok(nullptr, " \t"), &addr)) { printfln("ERR usage: RAWGET <A-D>"); return; }
    uint16_t code = dac.readCode(addr);
    uint16_t range = dac.readRegister(AD5724R_REG_RANGE, addr) & 0x07;
    uint16_t func = dac.readRegister(AD5724R_REG_CONTROL, AD5724R_CTRL_FUNCTION) & 0x0F;
    printfln("OK dac%c code=%u range=%u func=0x%X vdac=%.3f", 'A' + addr, code, range, func,
             AD5724R::codeToDacVolts(code));
  }
  else if (strcmp(cmd, "ADC") == 0)
  {
    long in; int16_t code;
    if (!parseLong(strtok(nullptr, " \t"), &in) || in < 0 || in > 3) { printfln("ERR usage: ADC <0-3>"); return; }
    if (!adc.readAveraged((uint8_t)in, ADC_AVERAGE, &code)) { printfln("ERR ADS1015 no response"); return; }
    printfln("OK ain%ld code=%d v=%.3f", in, code, ADS1015::codeToVolts(code));
  }
  else if (strcmp(cmd, "DFU") == 0)
  {
    printfln("OK entering bootloader");
    Serial.flush();
    delay(50);
    __wrap_enterSerialDfu();
  }
  else if (strcmp(cmd, "RAMTEST") == 0)
  {
    // Diagnostic: does the .noinit word survive a soft reset / a watchdog
    // reset? Afterwards ID shows bootword=0x54455354 if it did.
    char *a = strtok(nullptr, " 	");
    if (a) for (char *p = a; *p; p++) *p = (char)toupper((unsigned char)*p);
    if (a == nullptr || (strcmp(a, "SOFT") != 0 && strcmp(a, "WDT") != 0)) { printfln("ERR usage: RAMTEST <SOFT|WDT>"); return; }
    printfln("OK resetting (%s)", a);
    Serial.flush();
    delay(50);
    if (dacOk) zeroAllOutputs();
    railSet(false);
    dfuRequest = RAM_TEST_MAGIC;
    if (a[0] == 'S') NVIC_SystemReset();
    __disable_irq();
    for (;;) { }
  }
  else if (strcmp(cmd, "PINTEST") == 0)
  {
    // Bench only: hold one DAC-side MCU pin at a level so it can be checked
    // with a DMM at U3, or read the MISO input. SCK/MOSI take the SPI
    // peripheral offline; START (or SPIMODE) brings it back.
    char *a = strtok(nullptr, " 	");
    char *b = strtok(nullptr, " 	");
    if (a) for (char *p = a; *p; p++) *p = (char)toupper((unsigned char)*p);
    if (a == nullptr) { printfln("ERR usage: PINTEST <SYNC|CLR|SCK|MOSI> <0|1> | PINTEST MISO"); return; }
    if (strcmp(a, "MISO") == 0) { pinMode(PIN_SPI_MISO, INPUT); printfln("OK miso=%d", digitalRead(PIN_SPI_MISO)); return; }
    long v;
    if (!parseLong(b, &v) || (v != 0 && v != 1)) { printfln("ERR usage: PINTEST <SYNC|CLR|SCK|MOSI> <0|1>"); return; }
    uint8_t pin;
    if (strcmp(a, "SYNC") == 0) pin = PIN_DAC_SYNC;
    else if (strcmp(a, "CLR") == 0) pin = PIN_DAC_CLR;
    else if (strcmp(a, "SCK") == 0) { SPI.end(); pin = PIN_SPI_SCK; }
    else if (strcmp(a, "MOSI") == 0) { SPI.end(); pin = PIN_SPI_MOSI; }
    else { printfln("ERR unknown pin %s", a); return; }
    pinMode(pin, OUTPUT);
    digitalWrite(pin, v ? HIGH : LOW);
    printfln("OK %s=%ld (D%u)", a, v, pin);
  }
  else if (strcmp(cmd, "HANG") == 0)
  {
    printfln("OK hanging");
    Serial.flush();
    for (;;) { }   // watchdog test: rail drops and DAC clears within WDT_TIMEOUT_MS
  }
  else printfln("ERR unknown command: %s", cmd);
}

// --- entering the bootloader with the watchdog running ---------------------
// The nRF52 WDT keeps running through the soft reset the core uses for the
// 1200-baud touch, the XIAO's stock bootloader (0.6.1) does not feed it, and
// a watchdog reset wipes GPREGRET (the bootloader's DFU-request register), so
// neither a plain soft reset nor "set the magic and let the WDT fire" reaches
// serial DFU. What works is a double hop:
//   1. put the board in its safe state, leave a request word in .noinit RAM
//      (RAM survives a watchdog reset), stop kicking -> watchdog reset, which
//      is the one reset that stops the WDT;
//   2. the freshly booted firmware sees the request before it starts
//      anything, clears it and performs the core's normal soft reset into the
//      bootloader with the WDT now off.
// The core's enterSerialDfu() is redirected here with -Wl,--wrap (platformio.ini)
// so the 1200-baud touch from `pio run -t upload` takes the same path as the
// DFU host command.

static void leaveRequest(uint32_t word)
{
  dfuRequest = word;
  if (word == DFU_REQUEST_MAGIC)
  {
    // Flash copy: survives whatever the bootloader does to RAM in between.
    Adafruit_LittleFS_Namespace::File f(InternalFS);
    if (f.open(DFU_REQUEST_PATH, Adafruit_LittleFS_Namespace::FILE_O_WRITE)) { f.write((const uint8_t *)&word, sizeof(word)); f.close(); }
  }
}

extern "C" void __real_enterSerialDfu(void);
extern "C" void __wrap_enterSerialDfu(void)
{
  // Safe state first, in the shutdown order.
  if (dac.clearReleased()) { for (uint8_t a = 0; a < AD5724R_NUM_OUTPUTS; a++) dac.writeCode(a, 0); }
  digitalWrite(PIN_EFUSE_SHDN, LOW);
  dac.clearAssert();
  if (!(NRF_WDT->RUNSTATUS & WDT_RUNSTATUS_RUNSTATUS_Msk)) { __real_enterSerialDfu(); }
  leaveRequest(DFU_REQUEST_MAGIC);
  __disable_irq();
  for (;;) { }                  // watchdog reset within WDT_TIMEOUT_MS
}

//! First thing in setup(), after the filesystem is mounted and before the
//! watchdog starts: second hop of a DFU request. Consumes the request in
//! both places, so it can never loop.
static void honourDfuRequest()
{
  resetReason = NRF_POWER->RESETREAS;
  bootWord = dfuRequest;
  bool requested = (dfuRequest == DFU_REQUEST_MAGIC);
  dfuRequest = 0;
  if (InternalFS.exists(DFU_REQUEST_PATH))
  {
    InternalFS.remove(DFU_REQUEST_PATH);
    requested = true;
  }
  if (requested) __real_enterSerialDfu();   // never returns
}

// --- setup / loop ----------------------------------------------------------
void setup()
{
  // 1. GPIOs first, reproducing the pull-defined safe state explicitly.
  digitalWrite(PIN_EFUSE_SHDN, LOW);
  pinMode(PIN_EFUSE_SHDN, OUTPUT);
  digitalWrite(PIN_EFUSE_SHDN, LOW);
  pinMode(PIN_EFUSE_FLT, INPUT);          // 100 k pull-up on board
  railCmd = false;

  store.begin();                          // internal flash: calibration + channel map
  honourDfuRequest();                     // before the watchdog starts: second hop of a DFU request

  // 2. Buses. The core enables the nRF52's internal I2C pull-ups; the board
  //    has 4.7 k already, so disable them again (pin config only, TWIM untouched).
  dac.begin(PIN_DAC_SYNC, PIN_DAC_CLR, store.cfg().spiMode, 1000000UL);
  Wire.begin();
  Wire.setClock(400000);
  for (uint8_t p = 0; p < 2; p++)
  {
    uint32_t pin = g_ADigitalPinMap[p == 0 ? PIN_WIRE_SDA : PIN_WIRE_SCL];
    NRF_GPIO_Type *port = (pin < 32) ? NRF_P0 : NRF_P1;
    uint32_t cnf = port->PIN_CNF[pin & 31];
    cnf = (cnf & ~GPIO_PIN_CNF_PULL_Msk) | (GPIO_PIN_CNF_PULL_Disabled << GPIO_PIN_CNF_PULL_Pos);
    port->PIN_CNF[pin & 31] = cnf;
  }
  adc.begin(ADS1015_ADDR_GND, &Wire);

  Serial.begin(115200);

  // 3. Watchdog before anything can go wrong.
  wdtStart();
  wdtKick();

  // 4-9. DAC out of its safe state, proven by readback.
  if (!dacBringUp())
  {
    state = BoardState::FAULT;
    faultReason = FaultReason::SPI;
    printfln("!FAULT SPI readback 0x%04X", dacPowerReadback);
    // Rail stays off. Host can retry with SPIMODE / START.
  }

  // 10. ADC ACK check.
  adcOk = adc.probe();
  if (!adcOk) printfln("!WARN ADS1015 not responding at 0x48");

  // 11-12. Rail on, settle, first sweep, READY.
  if (dacOk) railBringUp();

  lastHousekeepMs = millis();
  lastCmdMs = millis();
}

void loop()
{
  while (Serial.available())
  {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r')
    {
      if (lineLen > 0)
      {
        lineBuf[lineLen] = '\0';
        handleLine(lineBuf);
        lineLen = 0;
      }
    }
    else if (lineLen < sizeof(lineBuf) - 1)
    {
      lineBuf[lineLen++] = c;
    }
    else
    {
      lineLen = 0;
      printfln("ERR line too long");
    }
  }

  uint32_t now = millis();
  if ((uint32_t)(now - lastHousekeepMs) >= HOUSEKEEP_PERIOD_MS)
  {
    lastHousekeepMs = now;
    housekeeping();
    wdtKick();   // only after a successful pass through the main loop
  }
  delay(1);
}
