/*!
LTC2668 self-contained Arduino driver implementation.
See LTC2668.h for the frame format and attribution.
*/

#include "LTC2668.h"
#include <math.h>

static const float SPAN_MIN[5] = {0.0, 0.0, -5.0, -10.0, -2.5};
static const float SPAN_MAX[5] = {5.0, 10.0, 5.0, 10.0, 2.5};

void LTC2668::begin(uint8_t csPin, uint32_t spiClockHz)
{
  _cs = csPin;
  _spiSettings = SPISettings(spiClockHz, MSBFIRST, SPI_MODE0);
  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH);
  SPI.begin();
  _haveLastTx = false;
  _verified = false;
  // Power-on span depends on the MSPx jumpers; DC2025A default (SoftSpan)
  // powers up as 0V-5V, zero-scale.
  for (uint8_t i = 0; i < LTC2668_NUM_CHANNELS; i++) _span[i] = LTC2668_SPAN_0_TO_5V;
}

void LTC2668::write(uint8_t command, uint8_t address, uint16_t data)
{
  uint8_t tx[4], rx[4];
  tx[0] = 0;  // don't-care byte; makes room for the previous frame's echo on SDO
  tx[1] = command | (address & 0x0F);
  tx[2] = (uint8_t)(data >> 8);
  tx[3] = (uint8_t)(data & 0xFF);

  SPI.beginTransaction(_spiSettings);
  digitalWrite(_cs, LOW);
  for (uint8_t i = 0; i < 4; i++) rx[i] = SPI.transfer(tx[i]);
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();

  // With a 32-bit frame, SDO shifts out the previous 24-bit command while the
  // new one shifts in: rx[1..3] should equal the previously transmitted frame.
  _verified = _haveLastTx &&
              rx[1] == _lastTx[0] && rx[2] == _lastTx[1] && rx[3] == _lastTx[2];
  _lastTx[0] = tx[1];
  _lastTx[1] = tx[2];
  _lastTx[2] = tx[3];
  _haveLastTx = true;
}

void LTC2668::setSpan(uint8_t channel, uint16_t span)
{
  channel &= 0x0F;
  write(LTC2668_CMD_SPAN, channel, span);
  _span[channel] = span;
}

void LTC2668::setSpanAll(uint16_t span)
{
  write(LTC2668_CMD_SPAN_ALL, 0, span);
  for (uint8_t i = 0; i < LTC2668_NUM_CHANNELS; i++) _span[i] = span;
}

void LTC2668::setCode(uint8_t channel, uint16_t code)
{
  channel &= 0x0F;
  _code[channel] = code;
  write(LTC2668_CMD_WRITE_N_UPDATE_N, channel, code);
}

float LTC2668::voltage(uint8_t channel) const
{
  channel &= 0x0F;
  return codeToVoltage(_code[channel], spanMin(_span[channel]), spanMax(_span[channel]));
}

float LTC2668::setVoltage(uint8_t channel, float volts)
{
  channel &= 0x0F;
  float vMin = spanMin(_span[channel]);
  float vMax = spanMax(_span[channel]);
  if (volts < vMin) volts = vMin;
  if (volts > vMax) volts = vMax;
  uint16_t code = voltageToCode(volts, vMin, vMax);
  setCode(channel, code);
  return codeToVoltage(code, vMin, vMax);
}

void LTC2668::setVoltageAll(float volts)
{
  for (uint8_t i = 0; i < LTC2668_NUM_CHANNELS; i++) setVoltage(i, volts);
}

void LTC2668::powerDown(uint8_t channel)
{
  write(LTC2668_CMD_POWER_DOWN_N, channel, 0);
}

void LTC2668::powerDownAll()
{
  write(LTC2668_CMD_POWER_DOWN_ALL, 0, 0);
}

void LTC2668::muxEnable(uint8_t channel)
{
  write(LTC2668_CMD_MUX, 0, LTC2668_MUX_ENABLE | (channel & 0x0F));
}

void LTC2668::muxDisable()
{
  write(LTC2668_CMD_MUX, 0, LTC2668_MUX_DISABLE);
}

float LTC2668::spanMin(uint16_t span)
{
  return (span < 5) ? SPAN_MIN[span] : 0.0;
}

float LTC2668::spanMax(uint16_t span)
{
  return (span < 5) ? SPAN_MAX[span] : 0.0;
}

uint16_t LTC2668::voltageToCode(float volts, float vMin, float vMax)
{
  float code = 65535.0 * (volts - vMin) / (vMax - vMin);
  code = (code > (floor(code) + 0.5)) ? ceil(code) : floor(code);
  if (code < 0.0) code = 0.0;
  if (code > 65535.0) code = 65535.0;
  return (uint16_t)code;
}

float LTC2668::codeToVoltage(uint16_t code, float vMin, float vMax)
{
  return ((float)code / 65535.0) * (vMax - vMin) + vMin;
}
