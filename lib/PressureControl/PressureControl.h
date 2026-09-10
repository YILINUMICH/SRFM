/*!
PressureControl: pressure <-> valve command voltage <-> DAC code, per channel.

Each valve has a linear calibration
    voltage = vMin + (pressure - pMin) * (vMax - vMin) / (pMax - pMin)
so any command-voltage regulator (0-10 V, 0-5 V, 1-5 V, inverted, ...) is
described by its two endpoints. Vacuum regulators have pMin > pMax and the
clamp uses min/max of the two.

The DAC side is a fraction of full scale: 10.000 V at the valve is
codeFullScale (per channel, from ValveConfig), so
    code = round(fraction * codeFullScale),  fraction = volts / 10
and the DAC pin sits slightly above the valve voltage because of the 100 ohm
series resistor -- that is exactly what codeFullScale absorbs.

Pure math on a ChannelCal; no hardware access. Defaults (kPa):
  CH1-CH3  SMC ITV2090-312L5 vacuum  0..10 V -> -1.3 .. -80 kPa
  CH4      SMC ITV0030-3BL   air     0..10 V -> +1 .. +500 kPa
*/

#ifndef PRESSURE_CONTROL_H
#define PRESSURE_CONTROL_H

#include <Arduino.h>
#include "ValveConfig.h"

#define VALVE_VOLTS_MAX 10.0f

namespace PressureControl
{
  //! Clamp a pressure to the calibrated range.
  float clampPressure(const ChannelCal &c, float p);

  //! Pressure -> valve volts (0..10, clamped).
  float pressureToVolts(const ChannelCal &c, float p);

  //! Valve volts -> pressure (inverse of the mapping, no clamp).
  float voltsToPressure(const ChannelCal &c, float v);

  //! Fraction of full scale (0..1) -> DAC code, clamped to codeFullScale.
  uint16_t fractionToCode(const ChannelCal &c, float fraction);

  //! DAC code -> fraction of full scale (0..1).
  float codeToFraction(const ChannelCal &c, uint16_t code);

  //! Valve volts for a DAC code.
  inline float codeToVolts(const ChannelCal &c, uint16_t code) { return codeToFraction(c, code) * VALVE_VOLTS_MAX; }

  //! Monitor ADC code -> monitor volts -> fraction of full scale. The SMC
  //! monitor is 1 V at 0 % and 5 V at 100 %.
  float adcCodeToMonitorVolts(const ChannelCal &c, int16_t code);
  inline float monitorVoltsToFraction(float vMon) { return (vMon - 1.0f) / 4.0f; }
}

#endif  // PRESSURE_CONTROL_H
