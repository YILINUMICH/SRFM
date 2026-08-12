/*!
LTC2668: 16-Channel SPI 16-Bit +/-10V Vout SoftSpan DAC driver.

Self-contained Arduino driver for the LTC2668-16 (as fitted on the DC2025A
demo board). Uses the Arduino SPI library directly — no Linduino/QuikEval
shield required.

SPI frame (MSB first, mode 0, CS/LD active low), 32-bit variant:
  Byte 1: don't care (enables previous-frame echo on SDO for verification)
  Byte 2: C3 C2 C1 C0 A3 A2 A1 A0   (command | channel address)
  Byte 3: D15..D8
  Byte 4: D7..D0

Command codes and span codes taken from the Analog Devices Linduino
LTC2668 library, Copyright 2018(c) Analog Devices, Inc.
*/

#ifndef LTC2668_H
#define LTC2668_H

#include <Arduino.h>
#include <SPI.h>

//! @name LTC2668 Command Codes (OR'd with the DAC address to form the command byte)
//! @{
#define  LTC2668_CMD_WRITE_N              0x00  //!< Write to input register n
#define  LTC2668_CMD_UPDATE_N             0x10  //!< Update (power up) DAC register n
#define  LTC2668_CMD_WRITE_N_UPDATE_ALL   0x20  //!< Write to input register n, update (power-up) all
#define  LTC2668_CMD_WRITE_N_UPDATE_N     0x30  //!< Write to input register n, update (power-up) n
#define  LTC2668_CMD_POWER_DOWN_N         0x40  //!< Power down n
#define  LTC2668_CMD_POWER_DOWN_ALL       0x50  //!< Power down chip (all DACs, MUX and reference)
#define  LTC2668_CMD_SPAN                 0x60  //!< Write span to DAC n
#define  LTC2668_CMD_CONFIG               0x70  //!< Configure reference / toggle
#define  LTC2668_CMD_WRITE_ALL            0x80  //!< Write to all input registers
#define  LTC2668_CMD_UPDATE_ALL           0x90  //!< Update all DACs
#define  LTC2668_CMD_WRITE_ALL_UPDATE_ALL 0xA0  //!< Write to all input registers, update all DACs
#define  LTC2668_CMD_MUX                  0xB0  //!< Select MUX channel (5 LSBs of data word)
#define  LTC2668_CMD_TOGGLE_SEL           0xC0  //!< Select which DACs can be toggled
#define  LTC2668_CMD_GLOBAL_TOGGLE        0xD0  //!< Software toggle via global toggle bit
#define  LTC2668_CMD_SPAN_ALL             0xE0  //!< Set span for all DACs
#define  LTC2668_CMD_NO_OPERATION         0xF0  //!< No operation
//! @}

//! @name LTC2668 Span Codes (valid for the internal 2.5V reference)
//! @{
#define  LTC2668_SPAN_0_TO_5V             0x0000
#define  LTC2668_SPAN_0_TO_10V            0x0001
#define  LTC2668_SPAN_PLUS_MINUS_5V       0x0002
#define  LTC2668_SPAN_PLUS_MINUS_10V      0x0003
#define  LTC2668_SPAN_PLUS_MINUS_2V5      0x0004
//! @}

//! @name Config command option bits (LTC2668_CMD_CONFIG)
//! @{
#define  LTC2668_REF_DISABLE              0x0001  //!< Disable internal reference (external ref in use)
#define  LTC2668_THERMAL_SHUTDOWN         0x0002  //!< Disable thermal shutdown (NOT recommended)
//! @}

//! @name MUX control (LTC2668_CMD_MUX)
//! @{
#define  LTC2668_MUX_DISABLE              0x0000
#define  LTC2668_MUX_ENABLE               0x0010  //!< OR with the channel number to monitor
//! @}

#define  LTC2668_NUM_CHANNELS             16

class LTC2668
{
public:
  //! Initialize SPI and the chip-select pin. Call once from setup().
  //! spiClockHz: 1 MHz default is conservative for jumper-wire connections
  //! (the part itself is good to 50 MHz).
  void begin(uint8_t csPin, uint32_t spiClockHz = 1000000UL);

  //! Set the SoftSpan range for one channel / all channels.
  //! Span tracking is kept internally so voltage conversions are correct.
  void setSpan(uint8_t channel, uint16_t span);
  void setSpanAll(uint16_t span);

  //! Write a raw 16-bit code to a channel and update its output immediately.
  void setCode(uint8_t channel, uint16_t code);

  //! Set a channel's output voltage (clamped to the channel's current span).
  //! Returns the voltage actually programmed after clamping/quantization.
  float setVoltage(uint8_t channel, float volts);

  //! Set every channel to the given voltage (e.g. 0.0 for a safe state).
  void setVoltageAll(float volts);

  //! Power down one channel / the whole chip.
  void powerDown(uint8_t channel);
  void powerDownAll();

  //! Route a DAC channel to the MUX pin, or disable the MUX.
  void muxEnable(uint8_t channel);
  void muxDisable();

  //! True if the last SPI frame echoed back the previously sent frame,
  //! i.e. the SDO readback verifies the bus is working. The first write
  //! after begin() has nothing to compare against and reports false.
  bool lastWriteVerified() const { return _verified; }

  //! Span limits and code/voltage conversion for a given span code.
  static float spanMin(uint16_t span);
  static float spanMax(uint16_t span);
  static uint16_t voltageToCode(float volts, float vMin, float vMax);
  static float codeToVoltage(uint16_t code, float vMin, float vMax);

  //! Current span code of a channel (as tracked by this driver).
  uint16_t span(uint8_t channel) const { return _span[channel & 0x0F]; }

  //! Last code written to a channel, and the voltage that code represents
  //! under the channel's current span. This is the driver's record of what
  //! the hardware was actually told, so it stays correct whether the channel
  //! was set via setVoltage(), setCode(), or a regulator pressure command.
  uint16_t code(uint8_t channel) const { return _code[channel & 0x0F]; }
  float    voltage(uint8_t channel) const;

private:
  //! Send one 32-bit frame; updates _verified from the SDO echo.
  void write(uint8_t command, uint8_t address, uint16_t data);

  uint8_t     _cs = 0xFF;
  SPISettings _spiSettings;
  uint16_t    _span[LTC2668_NUM_CHANNELS] = {0};
  uint16_t    _code[LTC2668_NUM_CHANNELS] = {0};
  uint8_t     _lastTx[3] = {0};
  bool        _haveLastTx = false;
  bool        _verified = false;
};

#endif  // LTC2668_H
