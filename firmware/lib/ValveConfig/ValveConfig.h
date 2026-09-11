/*!
ValveConfig: everything about the board that is data rather than code, kept
in one struct with a version and a CRC and persisted in the nRF52840's
internal flash (LittleFS via InternalFileSystem, file /srfm_cal.bin).

Per logical channel (CH1..CH4, index 0..3):
  dacAddr        AD5724R output (0=A .. 3=D) wired to this channel's pads
  adcInput       ADS1015 input (0..3) wired to this channel's monitor pin
  codeFullScale  DAC code that puts 10.000 V at the valve (absorbs the 100 ohm
                 series resistor against the valve's input impedance)
  rbGain_mV      monitor volts per ADC code, in mV (divider + ADC input Z)
  rbOffset       ADC code at 0 V monitor (normally 0)
  rbEnabled      0 if the valve has no monitor output (CH4 ITV0030 may not)
  vMin/vMax/pMin/pMax  pressure <-> valve-voltage endpoints (kPa / V)

Board-wide: heartbeat timeout and the SPI mode that passed the readback
proof on the bench.

The channel map defaults come from the Gerber review in FIRMWARE_HANDOFF.md
section 5; they are a starting point that the bench measurement replaces.
*/

#ifndef VALVE_CONFIG_H
#define VALVE_CONFIG_H

#include <Arduino.h>

#define VALVE_NUM_CHANNELS 4

struct ChannelCal
{
  uint8_t  dacAddr;
  uint8_t  adcInput;
  uint8_t  rbEnabled;
  uint8_t  reserved;
  uint16_t codeFullScale;
  int16_t  rbOffset;
  float    rbGain_mV;
  float    vMin;
  float    vMax;
  float    pMin;
  float    pMax;
};

struct ValveConfig
{
  uint32_t   magic;
  uint16_t   version;
  uint16_t   length;
  ChannelCal ch[VALVE_NUM_CHANNELS];
  uint16_t   hbTimeout_s;   // 0 = heartbeat disabled
  uint8_t    spiMode;       // SPI_MODE1 or SPI_MODE2
  uint8_t    reserved;
  uint32_t   crc;           // CRC-32 over everything above
};

class ValveConfigStore
{
public:
  //! Mount the internal filesystem and load; falls back to defaults and
  //! reports calibrated() == false when nothing valid is stored.
  void begin();

  ValveConfig &cfg() { return _cfg; }
  const ValveConfig &cfg() const { return _cfg; }

  bool calibrated() const { return _calibrated; }

  void setDefaults();
  bool load();
  bool save();

  static const char *channelName(uint8_t ch);   // "VAC1" .. "AIR"
  static uint32_t crc32(const uint8_t *data, size_t len);

private:
  void finalize();   // fill magic/version/length/crc
  ValveConfig _cfg;
  bool        _calibrated = false;
};

#endif  // VALVE_CONFIG_H
