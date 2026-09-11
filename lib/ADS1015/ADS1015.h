/*!
ADS1015: 4-channel 12-bit I2C ADC (TI SBAS473), used single-ended and
single-shot to read the valves' 1-5 V monitor outputs through the board's
0.3139 divider.

Config register (pointer 0x01), 16-bit big-endian:
  bit 15    OS     write 1 = start;  read 1 = idle/complete
  bit 14:12 MUX    100 AIN0  101 AIN1  110 AIN2  111 AIN3  (vs GND)
  bit 11:9  PGA    010 = +/-2.048 V FSR  (1 mV / code)   <- divider design
  bit 8     MODE   1 = single-shot
  bit 7:5   DR     100 = 1600 SPS (~0.6 ms per conversion, the default)
                   111 = 3300 SPS (~0.3 ms; used while streaming)
  bit 4:0   00011  comparator disabled (ALERT/RDY is unconnected)

Per conversion the I2C traffic (config write, OS poll, result read) costs
about as much as the conversion itself, so one single-ended reading is
~0.7 ms at 1600 SPS and ~0.55 ms at 3300 SPS.

Conversion register (pointer 0x00): 12-bit result left-justified in 16 bits.
*/

#ifndef ADS1015_H
#define ADS1015_H

#include <Arduino.h>
#include <Wire.h>

#define ADS1015_ADDR_GND       0x48
#define ADS1015_REG_CONVERSION 0x00
#define ADS1015_REG_CONFIG     0x01

#define ADS1015_MUX_AIN0  4
#define ADS1015_MUX_AIN1  5
#define ADS1015_MUX_AIN2  6
#define ADS1015_MUX_AIN3  7

#define ADS1015_CODE_MAX    2047
#define ADS1015_MV_PER_CODE 1.0f   // at +/-2.048 V FSR

#define ADS1015_DR_1600 4   // config DR field values (bits 7:5)
#define ADS1015_DR_3300 7

class ADS1015
{
public:
  void begin(uint8_t address = ADS1015_ADDR_GND, TwoWire *wire = &Wire);

  //! Conversion rate for subsequent readings: ADS1015_DR_1600 (default) or
  //! ADS1015_DR_3300. Takes effect on the next conversion.
  void setDataRate(uint8_t dr) { _dr = (uint8_t)(dr & 0x07); }
  uint8_t dataRate() const { return _dr; }

  //! Write a config with OS = 0 and check the ACK: I2C proof for bring-up.
  bool probe();

  //! One single-ended conversion on input 0..3 (or raw mux code 4..7).
  //! Returns false on an I2C error or a conversion timeout. Codes below 0
  //! (input slightly under ground) are clamped to 0.
  bool readSingle(uint8_t mux, int16_t *code);

  //! Average of n conversions. Mains rejection: 8-16 samples is the useful
  //! range against the front end's ~50 Hz corner.
  bool readAveraged(uint8_t mux, uint8_t n, int16_t *code);

  static float codeToVolts(int16_t code) { return code * ADS1015_MV_PER_CODE / 1000.0f; }

  //! Accepts 0..3 or the raw mux code 4..7; returns the raw mux code.
  static uint8_t muxCode(uint8_t input) { return input < 4 ? (uint8_t)(input + 4) : (uint8_t)(input & 0x07); }

private:
  bool writeConfig(uint16_t cfg);
  bool readReg(uint8_t reg, uint16_t *value);

  TwoWire *_wire = nullptr;
  uint8_t  _addr = ADS1015_ADDR_GND;
  uint8_t  _dr   = ADS1015_DR_1600;
};

#endif  // ADS1015_H
