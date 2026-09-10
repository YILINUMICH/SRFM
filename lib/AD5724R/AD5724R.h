/*!
AD5724R: quad 12-bit unipolar/bipolar SPI DAC (Analog Devices, Rev. G datasheet).

Register-level driver for the SRFM valve controller PCB. Everything the
firmware sends goes through 24-bit frames:

  DB23     R/W     0 = write, 1 = read
  DB22     0
  DB21:19  REG     000 DAC  001 output range  010 power control  011 control
  DB18:16  ADDR    000 A  001 B  010 C  011 D  100 all four
  DB15:0   DATA    12-bit codes are LEFT-justified: code << 4

SYNC (chip select, active low) frames the 24 clocks and the write takes
effect on its rising edge. LDAC is tied low on the board, so a DAC register
write updates the output immediately. CLR (active low) is pulled down on the
board: until firmware drives it high every SPI write is ignored and all
outputs sit at 0 V -- that is the hardware safe state.

SPI: MSB first, SCLK idles high, data latched on the falling edge = mode 2
(CPOL=1, CPHA=0). Mode 1 also latches on falling edges and usually works;
the mode is a begin() parameter so bring-up can try both and prove the
choice with readPowerControl().

Readback: a frame with DB23 = 1 selects a register; its contents are
clocked out on SDO during the *next* frame, which this driver makes a NOP.
*/

#ifndef AD5724R_H
#define AD5724R_H

#include <Arduino.h>
#include <SPI.h>

// Register field (DB21:19)
#define AD5724R_REG_DAC        0
#define AD5724R_REG_RANGE      1
#define AD5724R_REG_POWER      2
#define AD5724R_REG_CONTROL    3

// Address field (DB18:16)
#define AD5724R_ADDR_A         0
#define AD5724R_ADDR_B         1
#define AD5724R_ADDR_C         2
#define AD5724R_ADDR_D         3
#define AD5724R_ADDR_ALL       4

// Control-register functions (address field when REG = control)
#define AD5724R_CTRL_NOP       0
#define AD5724R_CTRL_FUNCTION  1
#define AD5724R_CTRL_CLEAR     4
#define AD5724R_CTRL_LOAD      5

// Control function bits (data word of CTRL_FUNCTION)
#define AD5724R_FUNC_SDO_DISABLE  0x0001
#define AD5724R_FUNC_CLR_SELECT   0x0002  // 1 = clear to mid-scale; keep 0 (clear to 0 V)
#define AD5724R_FUNC_CLAMP_ENABLE 0x0004  // 20 mA output current clamp
#define AD5724R_FUNC_TSD_ENABLE   0x0008  // thermal shutdown

// Output range codes (data DB2:0 of the range register)
#define AD5724R_RANGE_5V          0  // 0 .. +5 V
#define AD5724R_RANGE_10V         1  // 0 .. +10 V
#define AD5724R_RANGE_10V8        2  // 0 .. +10.8 V   <- used on this board
#define AD5724R_RANGE_PM5V        3
#define AD5724R_RANGE_PM10V       4
#define AD5724R_RANGE_PM10V8      5

// Power-control register bits
#define AD5724R_PU_A       0x0001
#define AD5724R_PU_B       0x0002
#define AD5724R_PU_C       0x0004
#define AD5724R_PU_D       0x0008
#define AD5724R_PU_REF     0x0010
#define AD5724R_PU_ALL     0x001F
#define AD5724R_TSD        0x0020  // read-only: thermal shutdown active
#define AD5724R_OC_A       0x0080  // read-only: output A current-clamped
#define AD5724R_OC_B       0x0100
#define AD5724R_OC_C       0x0200
#define AD5724R_OC_D       0x0400
#define AD5724R_OC_MASK    (AD5724R_OC_A | AD5724R_OC_B | AD5724R_OC_C | AD5724R_OC_D)

#define AD5724R_NUM_OUTPUTS     4
#define AD5724R_MAX_CODE        4095
#define AD5724R_FULL_SCALE_10V8 10.8f

class AD5724R
{
public:
  //! Configure the SYNC (CS) and CLR pins and the SPI settings. Leaves CLR
  //! asserted (low: outputs cleared, writes ignored) -- call clearRelease()
  //! as a deliberate step of the boot sequence. Does not touch the DAC.
  void begin(uint8_t syncPin, uint8_t clrPin, uint8_t spiMode = SPI_MODE2,
             uint32_t spiClockHz = 1000000UL);

  //! Change the SPI mode (SPI_MODE1 or SPI_MODE2) for bring-up experiments.
  void setSpiMode(uint8_t spiMode);
  uint8_t spiMode() const { return _spiMode; }

  //! CLR pin. Asserted (low): all outputs 0 V, SPI writes ignored.
  void clearAssert();
  void clearRelease();
  bool clearReleased() const { return _clrReleased; }

  // --- register writes -------------------------------------------------
  void writePowerControl(uint16_t bits);            // AD5724R_PU_ALL to run
  void writeRange(uint8_t addr, uint8_t range);     // addr may be ADDR_ALL
  void writeControlFunction(uint16_t funcBits);
  void softwareClear();                             // all outputs to 0 V
  void writeCode(uint8_t addr, uint16_t code);      // 0..4095, addr A..D

  // --- readback ----------------------------------------------------------
  //! Read any register. Returns the 16-bit data word clocked out during the
  //! following NOP frame. Requires MISO wired to SDO (it is, on this board).
  uint16_t readRegister(uint8_t reg, uint8_t addr);
  uint16_t readPowerControl() { return readRegister(AD5724R_REG_POWER, 0); }
  uint16_t readCode(uint8_t addr);                  // 12-bit, right-justified

  //! Boot-time proof of the SPI link: after writePowerControl(AD5724R_PU_ALL)
  //! the readback must show all five power-up bits. Also the VERIFY command.
  bool verifyPowerUp(uint16_t *readbackOut = nullptr);

  //! Last code this driver wrote to each output (A..D).
  uint16_t code(uint8_t addr) const { return _code[addr & 3]; }

  //! Voltage at the DAC pin for a code in the +10.8 V range (before the
  //! 100 ohm series resistor into the valve).
  static float codeToDacVolts(uint16_t code) { return code * AD5724R_FULL_SCALE_10V8 / 4096.0f; }

private:
  void transfer(uint8_t b0, uint16_t data, uint8_t *rx = nullptr);

  uint8_t     _sync = 0xFF;
  uint8_t     _clr = 0xFF;
  uint8_t     _spiMode = SPI_MODE2;
  uint32_t    _clockHz = 1000000UL;
  SPISettings _settings;
  bool        _clrReleased = false;
  uint16_t    _code[AD5724R_NUM_OUTPUTS] = {0, 0, 0, 0};
};

#endif  // AD5724R_H
