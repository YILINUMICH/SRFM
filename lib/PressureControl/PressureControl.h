/*!
PressureControl: maps pressure/vacuum setpoints to LTC2668 output voltages.

Each regulator has a linear calibration
    voltage = vMin + (pressure - pMin) * (vMax - vMin) / (pMax - pMin)
so any command-voltage regulator (0-10V, 0-5V, 1-5V, inverted, ...) can be
described by its two endpoints. Setpoints are clamped to [pMin, pMax]
(or [pMax, pMin] for vacuum regulators whose pMin > pMax).

Default configuration (EDIT TO MATCH YOUR REGULATORS — see README):
  regulator 0: air pressure, DAC ch 0, 0V..10V  ->   0 .. +100  (e.g. kPa)
  regulator 1: vacuum,       DAC ch 1, 0V..10V  ->   0 .. -100  (e.g. kPa)
  regulator 2: vacuum,       DAC ch 2, 0V..10V  ->   0 .. -100
  regulator 3: vacuum,       DAC ch 3, 0V..10V  ->   0 .. -100
*/

#ifndef PRESSURE_CONTROL_H
#define PRESSURE_CONTROL_H

#include <Arduino.h>
#include "LTC2668.h"

#define NUM_REGULATORS 4

struct RegulatorConfig
{
  const char *name;    //!< short label used in status output
  uint8_t dacChannel;  //!< LTC2668 channel driving this regulator
  float vMin;          //!< DAC voltage at pMin
  float vMax;          //!< DAC voltage at pMax
  float pMin;          //!< pressure at vMin (engineering units, e.g. kPa)
  float pMax;          //!< pressure at vMax
};

class PressureController
{
public:
  //! dac must already have begin() called and spans configured.
  void begin(LTC2668 *dac);

  //! Command a pressure setpoint. Clamped to the calibrated range.
  //! Returns the setpoint actually applied, or NAN for a bad index.
  float setPressure(uint8_t reg, float pressure);

  //! Drive every regulator to its zero-pressure voltage.
  void zeroAll();

  //! Update a regulator's calibration at runtime. Returns false on bad args.
  bool setCalibration(uint8_t reg, float vMin, float vMax, float pMin, float pMax);

  float pressureSetpoint(uint8_t reg) const { return _setpoint[reg]; }
  float voltageSetpoint(uint8_t reg) const { return _voltage[reg]; }
  const RegulatorConfig &config(uint8_t reg) const { return _cfg[reg]; }

private:
  LTC2668        *_dac = nullptr;
  RegulatorConfig _cfg[NUM_REGULATORS] = {
    {"AIR",  0, 0.0, 10.0, 0.0,  100.0},
    {"VAC1", 1, 0.0, 10.0, 0.0, -100.0},
    {"VAC2", 2, 0.0, 10.0, 0.0, -100.0},
    {"VAC3", 3, 0.0, 10.0, 0.0, -100.0},
  };
  float _setpoint[NUM_REGULATORS] = {0};
  float _voltage[NUM_REGULATORS] = {0};
};

#endif  // PRESSURE_CONTROL_H
