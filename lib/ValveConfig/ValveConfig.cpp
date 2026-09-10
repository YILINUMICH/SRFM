#include "ValveConfig.h"
#include <SPI.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

using namespace Adafruit_LittleFS_Namespace;

static const char    *CAL_PATH    = "/srfm_cal.bin";
static const uint32_t CAL_MAGIC   = 0x4C415653;   // "SVAL"
static const uint16_t CAL_VERSION = 1;

static const char *NAMES[VALVE_NUM_CHANNELS] = { "VAC1", "VAC2", "VAC3", "AIR" };

const char *ValveConfigStore::channelName(uint8_t ch)
{
  return ch < VALVE_NUM_CHANNELS ? NAMES[ch] : "?";
}

void ValveConfigStore::setDefaults()
{
  memset(&_cfg, 0, sizeof(_cfg));

  // Expected physical map from the Gerber review (FIRMWARE_HANDOFF section 5):
  // CH1 -> VOUT B / AIN3, CH2 -> VOUT A / AIN2, CH3 -> VOUT C / AIN1, CH4 -> VOUT D / AIN0.
  static const uint8_t DAC_ADDR[VALVE_NUM_CHANNELS] = { 1, 0, 2, 3 };
  static const uint8_t ADC_IN[VALVE_NUM_CHANNELS]   = { 3, 2, 1, 0 };

  for (uint8_t i = 0; i < VALVE_NUM_CHANNELS; i++)
  {
    ChannelCal &c = _cfg.ch[i];
    c.dacAddr   = DAC_ADDR[i];
    c.adcInput  = ADC_IN[i];
    c.rbEnabled = 1;
    c.rbGain_mV = 3.19f;   // 1 mV/code at the ADC through the 0.3139 divider
    c.rbOffset  = 0;
    c.vMin = 0.0f;
    c.vMax = 10.0f;
    if (i < 3)
    {
      // SMC ITV2090-312L5 vacuum: Z_in ~6.5 k, 0..10 V -> -1.3 .. -80 kPa
      c.codeFullScale = 3851;
      c.pMin = -1.3f;
      c.pMax = -80.0f;
    }
    else
    {
      // SMC ITV0030-3BL air: Z_in ~10 k, 0..10 V -> +1 .. +500 kPa
      c.codeFullScale = 3831;
      c.pMin = 1.0f;
      c.pMax = 500.0f;
    }
  }
  _cfg.hbTimeout_s = 2;
  _cfg.spiMode = SPI_MODE2;
  finalize();
}

void ValveConfigStore::finalize()
{
  _cfg.magic = CAL_MAGIC;
  _cfg.version = CAL_VERSION;
  _cfg.length = sizeof(ValveConfig);
  _cfg.crc = crc32((const uint8_t *)&_cfg, sizeof(ValveConfig) - sizeof(uint32_t));
}

void ValveConfigStore::begin()
{
  InternalFS.begin();
  if (!load())
  {
    setDefaults();
    _calibrated = false;
  }
}

bool ValveConfigStore::load()
{
  File f(InternalFS);
  if (!f.open(CAL_PATH, FILE_O_READ)) return false;

  ValveConfig tmp;
  int n = f.read(&tmp, sizeof(tmp));
  f.close();
  if (n != (int)sizeof(tmp)) return false;
  if (tmp.magic != CAL_MAGIC || tmp.version != CAL_VERSION || tmp.length != sizeof(ValveConfig)) return false;
  if (tmp.crc != crc32((const uint8_t *)&tmp, sizeof(ValveConfig) - sizeof(uint32_t))) return false;

  // Sanity-bound anything that could make the firmware misbehave if flash
  // were somehow valid-but-wrong.
  for (uint8_t i = 0; i < VALVE_NUM_CHANNELS; i++)
  {
    const ChannelCal &c = tmp.ch[i];
    if (c.dacAddr > 3 || c.adcInput > 3 || c.codeFullScale > 4095 || c.codeFullScale == 0) return false;
    if (!(c.rbGain_mV > 0.0f) || c.vMin == c.vMax || c.pMin == c.pMax) return false;
  }
  if (tmp.spiMode != SPI_MODE1 && tmp.spiMode != SPI_MODE2) return false;

  _cfg = tmp;
  _calibrated = true;
  return true;
}

bool ValveConfigStore::save()
{
  finalize();
  // LittleFS opens for write in append mode; start from a clean file.
  if (InternalFS.exists(CAL_PATH)) InternalFS.remove(CAL_PATH);

  File f(InternalFS);
  if (!f.open(CAL_PATH, FILE_O_WRITE)) return false;
  size_t n = f.write((const uint8_t *)&_cfg, sizeof(_cfg));
  f.close();
  if (n != sizeof(_cfg)) return false;
  _calibrated = true;
  return true;
}

uint32_t ValveConfigStore::crc32(const uint8_t *data, size_t len)
{
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; i++)
  {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
  }
  return ~crc;
}
