/*!
PressureControl: maps pressure/vacuum setpoints to LTC2668 output voltages.

Each regulator has a linear calibration
    voltage = vMin + (pressure - pMin) * (vMax - vMin) / (pMax - pMin)
so any command-voltage regulator (0-10V, 0-5V, 1-5V, inverted, ...) can be
described by its two endpoints. Setpoints are clamped to [pMin, pMax]
(or [pMax, pMin] for vacuum regulators whose pMin > pMax).

Default configuration, all pressures in kPa:
  regulator 0: SMC ITV0030-3BL   air pressure, ch 0, 0..10V -> +1 .. +500 kPa
  regulator 1: SMC ITV2090-312L5 vacuum,       ch 1, 0..10V -> -1.3 .. -80 kPa
  regulator 2: SMC ITV2090-312L5 vacuum,       ch 2, 0..10V -> -1.3 .. -80 kPa
  regulator 3: SMC ITV2090-312L5 vacuum,       ch 3, 0..10V -> -1.3 .. -80 kPa
(Ranges from the SMC datasheets; both models take a 0-10 VDC command signal.)
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

  //! Inverse of the setPressure() mapping: what pressure a given command
  //! voltage corresponds to. Lets status reporting derive pressure from the
  //! DAC's actual output instead of a cached setpoint, so a raw `V` command
  //! on a regulator channel is reported honestly.
  float pressureFromVoltage(uint8_t reg, float volts) const
  {
    if (reg >= NUM_REGULATORS) return NAN;
    const RegulatorConfig &c = _cfg[reg];
    return c.pMin + (volts - c.vMin) * (c.pMax - c.pMin) / (c.vMax - c.vMin);
  }
  const RegulatorConfig &config(uint8_t reg) const { return _cfg[reg]; }

private:
  LTC2668        *_dac = nullptr;
  RegulatorConfig _cfg[NUM_REGULATORS] = {
    {"AIR",  0, 0.0, 10.0,  1.0, 500.0},   // ITV0030-3BL: 0.001-0.5 MPa
    {"VAC1", 1, 0.0, 10.0, -1.3, -80.0},   // ITV2090-312L5
    {"VAC2", 2, 0.0, 10.0, -1.3, -80.0},   // ITV2090-312L5
    {"VAC3", 3, 0.0, 10.0, -1.3, -80.0},   // ITV2090-312L5
  };
  float _setpoint[NUM_REGULATORS] = {0};
  float _voltage[NUM_REGULATORS] = {0};
};

#endif  // PRESSURE_CONTROL_H
