#include "PressureControl.h"
#include <math.h>

namespace PressureControl
{

float clampPressure(const ChannelCal &c, float p)
{
  float lo = min(c.pMin, c.pMax);
  float hi = max(c.pMin, c.pMax);
  if (p < lo) p = lo;
  if (p > hi) p = hi;
  return p;
}

float pressureToVolts(const ChannelCal &c, float p)
{
  p = clampPressure(c, p);
  float v = c.vMin + (p - c.pMin) * (c.vMax - c.vMin) / (c.pMax - c.pMin);
  if (v < 0.0f) v = 0.0f;
  if (v > VALVE_VOLTS_MAX) v = VALVE_VOLTS_MAX;
  return v;
}

float voltsToPressure(const ChannelCal &c, float v)
{
  return c.pMin + (v - c.vMin) * (c.pMax - c.pMin) / (c.vMax - c.vMin);
}

uint16_t fractionToCode(const ChannelCal &c, float fraction)
{
  if (fraction < 0.0f) fraction = 0.0f;
  if (fraction > 1.0f) fraction = 1.0f;
  float code = roundf(fraction * (float)c.codeFullScale);
  if (code > (float)c.codeFullScale) code = (float)c.codeFullScale;
  return (uint16_t)code;
}

float codeToFraction(const ChannelCal &c, uint16_t code)
{
  if (c.codeFullScale == 0) return 0.0f;
  return (float)code / (float)c.codeFullScale;
}

float adcCodeToMonitorVolts(const ChannelCal &c, int16_t code)
{
  return (float)(code - c.rbOffset) * c.rbGain_mV / 1000.0f;
}

}  // namespace PressureControl
