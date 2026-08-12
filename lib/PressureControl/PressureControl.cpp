#include "PressureControl.h"

void PressureController::begin(LTC2668 *dac)
{
  _dac = dac;
  zeroAll();
}

float PressureController::setPressure(uint8_t reg, float pressure)
{
  if (reg >= NUM_REGULATORS || _dac == nullptr) return NAN;
  const RegulatorConfig &c = _cfg[reg];

  // Clamp to the calibrated range; handles vacuum configs where pMin > pMax.
  float lo = min(c.pMin, c.pMax);
  float hi = max(c.pMin, c.pMax);
  if (pressure < lo) pressure = lo;
  if (pressure > hi) pressure = hi;

  float volts = c.vMin + (pressure - c.pMin) * (c.vMax - c.vMin) / (c.pMax - c.pMin);
  _voltage[reg] = _dac->setVoltage(c.dacChannel, volts);
  _setpoint[reg] = pressure;
  return pressure;
}

void PressureController::zeroAll()
{
  for (uint8_t i = 0; i < NUM_REGULATORS; i++) setPressure(i, 0.0);
}

bool PressureController::setCalibration(uint8_t reg, float vMin, float vMax, float pMin, float pMax)
{
  if (reg >= NUM_REGULATORS) return false;
  if (vMin == vMax || pMin == pMax) return false;  // degenerate mapping
  _cfg[reg].vMin = vMin;
  _cfg[reg].vMax = vMax;
  _cfg[reg].pMin = pMin;
  _cfg[reg].pMax = pMax;
  // Re-apply the current setpoint under the new calibration.
  setPressure(reg, _setpoint[reg]);
  return true;
}
