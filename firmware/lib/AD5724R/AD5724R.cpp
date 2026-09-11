#include "AD5724R.h"

void AD5724R::begin(uint8_t syncPin, uint8_t clrPin, uint8_t spiMode, uint32_t spiClockHz)
{
  _sync = syncPin;
  _clr = clrPin;
  _clockHz = spiClockHz;

  // Reproduce what the board pulls were already doing (SYNC up, CLR down)
  // so nothing glitches when the pins become outputs.
  digitalWrite(_sync, HIGH);
  pinMode(_sync, OUTPUT);
  digitalWrite(_sync, HIGH);
  digitalWrite(_clr, LOW);
  pinMode(_clr, OUTPUT);
  digitalWrite(_clr, LOW);
  _clrReleased = false;

  SPI.begin();
  setSpiMode(spiMode);
  for (uint8_t i = 0; i < AD5724R_NUM_OUTPUTS; i++) _code[i] = 0;
}

void AD5724R::setSpiMode(uint8_t spiMode)
{
  _spiMode = spiMode;
  _settings = SPISettings(_clockHz, MSBFIRST, spiMode);
}

void AD5724R::clearAssert()
{
  digitalWrite(_clr, LOW);
  _clrReleased = false;
  for (uint8_t i = 0; i < AD5724R_NUM_OUTPUTS; i++) _code[i] = 0;
}

void AD5724R::clearRelease()
{
  digitalWrite(_clr, HIGH);
  _clrReleased = true;
}

void AD5724R::transfer(uint8_t b0, uint16_t data, uint8_t *rx)
{
  uint8_t tx[3] = { b0, (uint8_t)(data >> 8), (uint8_t)(data & 0xFF) };
  uint8_t r[3];
  SPI.beginTransaction(_settings);
  digitalWrite(_sync, LOW);
  for (uint8_t i = 0; i < 3; i++) r[i] = SPI.transfer(tx[i]);
  digitalWrite(_sync, HIGH);   // write takes effect here; stays high >= 30 ns
  SPI.endTransaction();
  if (rx) { rx[0] = r[0]; rx[1] = r[1]; rx[2] = r[2]; }
}

static inline uint8_t frameByte(bool read, uint8_t reg, uint8_t addr)
{
  return (uint8_t)((read ? 0x80 : 0x00) | ((reg & 0x07) << 3) | (addr & 0x07));
}

void AD5724R::writePowerControl(uint16_t bits)
{
  transfer(frameByte(false, AD5724R_REG_POWER, 0), bits);
}

void AD5724R::writeRange(uint8_t addr, uint8_t range)
{
  transfer(frameByte(false, AD5724R_REG_RANGE, addr), range & 0x07);
}

void AD5724R::writeControlFunction(uint16_t funcBits)
{
  transfer(frameByte(false, AD5724R_REG_CONTROL, AD5724R_CTRL_FUNCTION), funcBits & 0x000F);
}

void AD5724R::softwareClear()
{
  transfer(frameByte(false, AD5724R_REG_CONTROL, AD5724R_CTRL_CLEAR), 0);
  for (uint8_t i = 0; i < AD5724R_NUM_OUTPUTS; i++) _code[i] = 0;
}

void AD5724R::writeCode(uint8_t addr, uint16_t code)
{
  addr &= 0x03;                       // never "all four" for value writes
  if (code > AD5724R_MAX_CODE) code = AD5724R_MAX_CODE;
  _code[addr] = code;
  transfer(frameByte(false, AD5724R_REG_DAC, addr), (uint16_t)(code << 4));
}

uint16_t AD5724R::readRegister(uint8_t reg, uint8_t addr)
{
  uint8_t rx[3];
  transfer(frameByte(true, reg, addr), 0);                                   // select
  transfer(frameByte(false, AD5724R_REG_CONTROL, AD5724R_CTRL_NOP), 0, rx);  // clock it out
  return (uint16_t)((rx[1] << 8) | rx[2]);
}

uint16_t AD5724R::readCode(uint8_t addr)
{
  return readRegister(AD5724R_REG_DAC, addr & 0x03) >> 4;
}

bool AD5724R::verifyPowerUp(uint16_t *readbackOut)
{
  uint16_t pc = readPowerControl();
  if (readbackOut) *readbackOut = pc;
  return (pc & AD5724R_PU_ALL) == AD5724R_PU_ALL;
}
