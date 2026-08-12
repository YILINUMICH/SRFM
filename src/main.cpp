/*
SRFM regulator controller — Arduino Mega 2560 + LTC2668 EVM (DC2025A).

Drives 1 air-pressure regulator + 3 vacuum regulators from LTC2668
channels 0-3 (all channels configured for the +/-10V SoftSpan range).

Serial protocol, 115200 baud, one ASCII command per line ('\n' terminated).
Every command gets exactly one "OK ..." or "ERR ..." reply line:

  ID                                    -> OK SRFM-DAC v1.0
  P <reg 0-3> <pressure>                set regulator pressure (eng. units)
  V <ch 0-15> <volts>                   set raw DAC voltage (-10..+10)
  CAL <reg> <vMin> <vMax> <pMin> <pMax> set regulator calibration
  GET                                   -> OK <name>=<p>,<V>V; ... per regulator
  ZERO                                  all regulators to 0, other channels 0V
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
  dac.setSpanAll(LTC2668_SPAN_PLUS_MINUS_10V);
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
    Serial.print(regulators.config(i).name);
    Serial.print('=');
    Serial.print(regulators.pressureSetpoint(i), 2);
    Serial.print(',');
    Serial.print(regulators.voltageSetpoint(i), 3);
    Serial.print('V');
    if (i < NUM_REGULATORS - 1) Serial.print(F("; "));
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
    Serial.println(applied, 3);
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
