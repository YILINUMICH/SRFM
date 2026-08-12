# SRFM Regulator Controller

Arduino Mega 2560 + **LTC2668 EVM (DC2025A)** 16-channel ±10V DAC, driving
**1 air-pressure regulator + 3 vacuum regulators**, with a Python GUI over
USB serial.

```
┌────────┐  USB serial   ┌──────────────┐  SPI   ┌──────────────┐  0..10V   ┌────────────┐
│ Python │ ────────────► │ Arduino Mega │ ─────► │ LTC2668 EVM  │ ────────► │ regulators │
│  GUI   │   115200 8N1  │  (firmware)  │        │  (DC2025A)   │  ch 0-3   │ 1 air+3 vac│
└────────┘               └──────────────┘        └──────────────┘           └────────────┘
```

## Project layout

| Path | What it is |
|---|---|
| `platformio.ini` | PlatformIO config (board `megaatmega2560`) |
| `lib/LTC2668/` | Self-contained LTC2668 SPI driver (`begin`, `setSpanAll`, `setVoltage`, …) |
| `lib/PressureControl/` | Voltage↔pressure linear mapping for the 4 regulators |
| `src/main.cpp` | Firmware: init, safe-state, serial command protocol |
| `gui/pressure_gui.py` | tkinter GUI (needs `pip install -r gui/requirements.txt`) |

## Wiring: Mega ↔ DC2025A

Use the SPI through-hole test points next to J1 (or J1 itself).

| Mega pin | DC2025A | Note |
|---|---|---|
| 52 (SCK) | SCK | |
| 51 (MOSI) | SDI | |
| 50 (MISO) | SDO | optional, enables SPI readback verification |
| 53 (CS) | CS/LD | change `DAC_CS_PIN` in `src/main.cpp` if you use another pin |
| 5V | **OVP** | required — powers the EVM's digital interface at Arduino logic level |
| GND | GND | common ground |

Analog supply (bench supply): **V+ = +15V, V− = −15V, GND** on the EVM turret
posts. ±15V is required for the ±10V SoftSpan range.

Jumper configuration (as set on our board — this is the factory default):

| Jumper | Function | Setting |
|---|---|---|
| JP1 | REF_SEL | **INT** (internal 2.5V reference) |
| JP2 | MSP0 | **1** |
| JP3 | MSP1 | **1** |
| JP4 | MSP2 | **1** |

MSP0/1/2 = 1/1/1 selects **SoftSpan mode**: powers up in the 0–5V span at
zero-scale (0V). The firmware then switches all channels to ±10V span and
0V output at boot. Do not change these jumpers — any other MSP setting
locks the part into a fixed manual span and the firmware's span commands
would be ignored.

Regulator command inputs connect to **VOUT0–VOUT3** (+ their grounds):
VOUT0 = air pressure, VOUT1–3 = vacuum.

## Build & flash

```sh
pio run                 # compile
pio run -t upload       # flash the Mega
pio device monitor      # 115200 baud, expect: "OK SRFM-DAC v1.0 ready"
```

## Serial protocol (115200, one command per line)

| Command | Meaning | Reply |
|---|---|---|
| `ID` | identify | `OK SRFM-DAC v1.0` |
| `P <reg 0-3> <pressure>` | set regulator pressure (eng. units) | `OK AIR p=50.00 v=5.000` |
| `V <ch 0-15> <volts>` | raw DAC voltage, −10…+10 | `OK ch4 v=-2.500` |
| `CAL <reg> <vMin> <vMax> <pMin> <pMax>` | set regulator calibration | `OK cal set` |
| `GET` | status of all 4 regulators | `OK AIR=0.00,0.000V; VAC1=…` |
| `ZERO` | everything to 0 / safe state | `OK all zero` |

Setpoints are clamped to the calibrated range; the reply always shows what
was actually applied.

## Calibration

Defaults (in `lib/PressureControl/PressureControl.h`) match the actual
regulators, all pressures in kPa:

| Reg | Model | Command | Pressure range |
|---|---|---|---|
| 0 AIR | SMC ITV0030-3BL | 0–10 VDC | +1 … +500 kPa (0.001–0.5 MPa) |
| 1–3 VAC | SMC ITV2090-312L5 | 0–10 VDC | −1.3 … −80 kPa |

Both models take a 0–10V command signal, so no signal conditioning is needed
between VOUT0–3 and the regulators. To tweak the mapping (e.g. after
verifying against a gauge), edit the header or use `CAL` at runtime; keep the
`REGULATORS` slider ranges at the top of `gui/pressure_gui.py` in sync.

## GUI

```sh
pip install -r gui/requirements.txt
python3 gui/pressure_gui.py
```

Pick the Mega's port (`usbmodem…` on macOS), Connect (waits ~2.5s for the
auto-reset), then use the sliders/entries. `ZERO ALL` is the panic button;
the GUI also sends `ZERO` on window close.
