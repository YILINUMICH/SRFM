/*
SRFM regulator controller — Arduino Mega 2560 + LTC2668 EVM (DC2025A).

Drives 1 air-pressure regulator + 3 vacuum regulators from LTC2668
channels 0-3 (all channels configured for the 0-10V range, which is what
the regulators accept; see setup() for why that span and not +/-10V).

Serial protocol, 115200 baud, one ASCII command per line ('\n' terminated).
Every command gets exactly one "OK ..." or "ERR ..." reply line.

Pressure layer (normal operation):
  ID                                    -> OK SRFM-DAC v1.0
  P <reg 0-3> <pressure>                set regulator pressure (eng. units)
  CAL <reg> <vMin> <vMax> <pMin> <pMax> set regulator calibration
  GET                                   -> OK <name>=<p>,<V>V; ... per regulator
  ZERO                                  all regulators to 0, other channels 0V

Raw DAC layer (bring-up and debugging, bypasses the pressure mapping):
  V <ch 0-15> <volts>                   set channel to a voltage in its span
  C <ch 0-15> <code 0-65535>            write a raw 16-bit DAC code
  SPAN <span>                           set span on all channels
  SPAN <ch> <span>                      set span on one channel
                                        0=0-5V 1=0-10V 2=+/-5V 3=+/-10V 4=+/-2.5V
  DUMP                                  -> OK ch0=<code>,<V>V,s<span>; ... all 16
  VERIFY                                -> OK verify=yes|no (SDO readback, pin 50)

GET and DUMP both report the DAC's own record of what was last written, so a
raw V or C command on a regulator channel is reflected honestly rather than
hidden behind a stale pressure setpoint.
*/

#include <Arduino.h>
#include "LTC2668.h"
#include "PressureControl.h"

static const uint8_t DAC_CS_PIN = 53;  // Mega hardware SS; SCK=52 MOSI=51 MISO=50

LTC2668 dac;
PressureController regulators;

static char lineBuf[96];
static uint8_t lineLen = 0;

static void handleLine(char *line);

void setup()
{
  Serial.begin(115200);

  dac.begin(DAC_CS_PIN);
  // All four regulators take a 0-10V command, so the 0-10V span is the right
  // one: it doubles resolution vs +/-10V and makes code 0 a true 0V output.
  // It is also the span the MSPx jumpers select on our board, so the code
  // conversion stays correct even when the part is in fixed (non-SoftSpan)
  // mode and ignores this command.
  dac.setSpanAll(LTC2668_SPAN_0_TO_10V);
  dac.setVoltageAll(0.0);      // safe state: every output at 0V
  regulators.begin(&dac);      // regulators to their zero-pressure voltage

  Serial.println(F("OK SRFM-DAC v1.0 ready"));
}

void loop()
{
  while (Serial.available())
  {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r')
    {
      if (lineLen > 0)
      {
        lineBuf[lineLen] = '\0';
        handleLine(lineBuf);
        lineLen = 0;
      }
    }
    else if (lineLen < sizeof(lineBuf) - 1)
    {
      lineBuf[lineLen++] = c;
    }
    else
    {
      lineLen = 0;  // overlong line: drop it
      Serial.println(F("ERR line too long"));
    }
  }
}

static void printStatus()
{
  Serial.print(F("OK "));
  for (uint8_t i = 0; i < NUM_REGULATORS; i++)
  {
    // Report what the DAC was actually last told, not the pressure-layer
    // cache: a raw `V`/`C` command on a regulator channel must not be able to
    // leave GET claiming a setpoint the hardware is not holding.
    float v = dac.voltage(regulators.config(i).dacChannel);
    Serial.print(regulators.config(i).name);
    Serial.print('=');
    Serial.print(regulators.pressureFromVoltage(i, v), 2);
    Serial.print(',');
    Serial.print(v, 3);
    Serial.print('V');
    if (i < NUM_REGULATORS - 1) Serial.print(F("; "));
  }
  Serial.println();
}

static void printDump()
{
  Serial.print(F("OK "));
  for (uint8_t ch = 0; ch < LTC2668_NUM_CHANNELS; ch++)
  {
    Serial.print(F("ch"));
    Serial.print(ch);
    Serial.print('=');
    Serial.print(dac.code(ch));
    Serial.print(',');
    Serial.print(dac.voltage(ch), 3);
    Serial.print(F("V,s"));
    Serial.print(dac.span(ch));
    if (ch < LTC2668_NUM_CHANNELS - 1) Serial.print(F("; "));
  }
  Serial.println();
}

static void handleLine(char *line)
{
  char *cmd = strtok(line, " \t");
  if (cmd == nullptr) return;
  for (char *p = cmd; *p; p++) *p = toupper(*p);

  if (strcmp(cmd, "ID") == 0)
  {
    Serial.println(F("OK SRFM-DAC v1.0"));
  }
  else if (strcmp(cmd, "P") == 0)
  {
    char *regStr = strtok(nullptr, " \t");
    char *valStr = strtok(nullptr, " \t");
    if (regStr == nullptr || valStr == nullptr)
    {
      Serial.println(F("ERR usage: P <reg 0-3> <pressure>"));
      return;
    }
    int reg = atoi(regStr);
    if (reg < 0 || reg >= NUM_REGULATORS)
    {
      Serial.println(F("ERR bad regulator index"));
      return;
    }
    float applied = regulators.setPressure((uint8_t)reg, atof(valStr));
    Serial.print(F("OK "));
    Serial.print(regulators.config(reg).name);
    Serial.print(F(" p="));
    Serial.print(applied, 2);
    Serial.print(F(" v="));
    Serial.println(regulators.voltageSetpoint(reg), 3);
  }
  else if (strcmp(cmd, "V") == 0)
  {
    char *chStr = strtok(nullptr, " \t");
    char *valStr = strtok(nullptr, " \t");
    if (chStr == nullptr || valStr == nullptr)
    {
      Serial.println(F("ERR usage: V <ch 0-15> <volts>"));
      return;
    }
    int ch = atoi(chStr);
    if (ch < 0 || ch >= LTC2668_NUM_CHANNELS)
    {
      Serial.println(F("ERR bad channel"));
      return;
    }
    float applied = dac.setVoltage((uint8_t)ch, atof(valStr));
    Serial.print(F("OK ch"));
    Serial.print(ch);
    Serial.print(F(" v="));
    Serial.print(applied, 3);
    Serial.print(F(" code="));
    Serial.println(dac.code((uint8_t)ch));
  }
  else if (strcmp(cmd, "C") == 0)
  {
    char *chStr = strtok(nullptr, " \t");
    char *valStr = strtok(nullptr, " \t");
    if (chStr == nullptr || valStr == nullptr)
    {
      Serial.println(F("ERR usage: C <ch 0-15> <code 0-65535>"));
      return;
    }
    int ch = atoi(chStr);
    long code = atol(valStr);
    if (ch < 0 || ch >= LTC2668_NUM_CHANNELS)
    {
      Serial.println(F("ERR bad channel"));
      return;
    }
    if (code < 0 || code > 65535)
    {
      Serial.println(F("ERR code out of range"));
      return;
    }
    dac.setCode((uint8_t)ch, (uint16_t)code);
    Serial.print(F("OK ch"));
    Serial.print(ch);
    Serial.print(F(" code="));
    Serial.print(dac.code((uint8_t)ch));
    Serial.print(F(" v="));
    Serial.println(dac.voltage((uint8_t)ch), 3);
  }
  else if (strcmp(cmd, "SPAN") == 0)
  {
    char *a = strtok(nullptr, " \t");
    char *b = strtok(nullptr, " \t");
    if (a == nullptr)
    {
      Serial.println(F("ERR usage: SPAN <span> | SPAN <ch> <span>"));
      return;
    }
    // One argument sets every channel; two sets a single channel.
    int ch = (b == nullptr) ? -1 : atoi(a);
    int span = atoi(b == nullptr ? a : b);
    if (span < 0 || span > 4)
    {
      Serial.println(F("ERR bad span (0=0-5V 1=0-10V 2=+/-5V 3=+/-10V 4=+/-2.5V)"));
      return;
    }
    if (b != nullptr && (ch < 0 || ch >= LTC2668_NUM_CHANNELS))
    {
      Serial.println(F("ERR bad channel"));
      return;
    }
    if (ch < 0) dac.setSpanAll((uint16_t)span);
    else        dac.setSpan((uint8_t)ch, (uint16_t)span);
    Serial.print(F("OK span="));
    Serial.print(span);
    Serial.print(F(" range="));
    Serial.print(LTC2668::spanMin((uint16_t)span), 2);
    Serial.print(F(".."));
    Serial.print(LTC2668::spanMax((uint16_t)span), 2);
    Serial.print(F("V on "));
    if (ch < 0) Serial.println(F("all channels"));
    else        { Serial.print(F("ch")); Serial.println(ch); }
  }
  else if (strcmp(cmd, "DUMP") == 0)
  {
    printDump();
  }
  else if (strcmp(cmd, "VERIFY") == 0)
  {
    // Sends a no-op purely to exercise the bus, then reports whether SDO
    // echoed the previous frame back. Requires MISO wired to the EVM's SDO.
    dac.setCode(0, dac.code(0));
    Serial.print(F("OK verify="));
    Serial.println(dac.lastWriteVerified() ? F("yes") : F("no"));
  }
  else if (strcmp(cmd, "CAL") == 0)
  {
    char *regStr = strtok(nullptr, " \t");
    char *a = strtok(nullptr, " \t");
    char *b = strtok(nullptr, " \t");
    char *c = strtok(nullptr, " \t");
    char *d = strtok(nullptr, " \t");
    if (regStr == nullptr || a == nullptr || b == nullptr || c == nullptr || d == nullptr)
    {
      Serial.println(F("ERR usage: CAL <reg> <vMin> <vMax> <pMin> <pMax>"));
      return;
    }
    int reg = atoi(regStr);
    if (reg < 0 || reg >= NUM_REGULATORS ||
        !regulators.setCalibration((uint8_t)reg, atof(a), atof(b), atof(c), atof(d)))
    {
      Serial.println(F("ERR bad calibration"));
      return;
    }
    Serial.println(F("OK cal set"));
  }
  else if (strcmp(cmd, "GET") == 0)
  {
    printStatus();
  }
  else if (strcmp(cmd, "ZERO") == 0)
  {
    dac.setVoltageAll(0.0);
    regulators.zeroAll();
    Serial.println(F("OK all zero"));
  }
  else
  {
    Serial.print(F("ERR unknown command: "));
    Serial.println(cmd);
  }
}
