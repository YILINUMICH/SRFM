#include "ADS1015.h"

// PGA 010 | single-shot | comparator off; OS, MUX and DR are filled per call.
static const uint16_t CFG_BASE = 0x0400 | 0x0100 | 0x0003;
static const uint16_t CFG_OS   = 0x8000;

void ADS1015::begin(uint8_t address, TwoWire *wire)
{
  _addr = address;
  _wire = wire;
}

bool ADS1015::writeConfig(uint16_t cfg)
{
  _wire->beginTransmission(_addr);
  _wire->write(ADS1015_REG_CONFIG);
  _wire->write((uint8_t)(cfg >> 8));
  _wire->write((uint8_t)(cfg & 0xFF));
  return _wire->endTransmission() == 0;
}

bool ADS1015::readReg(uint8_t reg, uint16_t *value)
{
  _wire->beginTransmission(_addr);
  _wire->write(reg);
  if (_wire->endTransmission() != 0) return false;
  if (_wire->requestFrom(_addr, (uint8_t)2) != 2) return false;
  uint8_t hi = (uint8_t)_wire->read();
  uint8_t lo = (uint8_t)_wire->read();
  *value = (uint16_t)((hi << 8) | lo);
  return true;
}

bool ADS1015::probe()
{
  return writeConfig(CFG_BASE | ((uint16_t)_dr << 5) | ((uint16_t)ADS1015_MUX_AIN0 << 12));
}

bool ADS1015::readSingle(uint8_t mux, int16_t *code)
{
  mux = muxCode(mux);
  if (!writeConfig(CFG_OS | CFG_BASE | ((uint16_t)_dr << 5) | ((uint16_t)mux << 12))) return false;

  // ~0.6 ms at 1600 SPS, ~0.3 ms at 3300; poll OS with a hard cap so a
  // wedged bus cannot stall the housekeeping loop (and with it the watchdog
  // kick).
  uint32_t t0 = micros();
  for (;;)
  {
    uint16_t cfg;
    if (!readReg(ADS1015_REG_CONFIG, &cfg)) return false;
    if (cfg & CFG_OS) break;
    if ((uint32_t)(micros() - t0) > 5000UL) return false;
    delayMicroseconds(100);
  }

  uint16_t raw;
  if (!readReg(ADS1015_REG_CONVERSION, &raw)) return false;
  int16_t c = (int16_t)((int16_t)raw >> 4);   // arithmetic shift keeps the sign
  if (c < 0) c = 0;                            // single-ended: below ground is noise
  *code = c;
  return true;
}

bool ADS1015::readAveraged(uint8_t mux, uint8_t n, int16_t *code)
{
  if (n == 0) n = 1;
  int32_t sum = 0;
  for (uint8_t i = 0; i < n; i++)
  {
    int16_t c;
    if (!readSingle(mux, &c)) return false;
    sum += c;
  }
  *code = (int16_t)((sum + n / 2) / n);
  return true;
}
