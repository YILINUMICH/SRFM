# HANDOFF — SRFM Regulator Controller

**Date:** 2026-08-12
**Status:** Firmware compiles clean; GUI syntax-checked. **Not yet tested on hardware.**

## 1. What this project is

A control chain for driving **1 air-pressure regulator and 3 vacuum regulators**
from a PC:

```
Python GUI ──USB serial (115200)──► Arduino Mega 2560 ──SPI──► LTC2668 EVM (DC2025A) ──0..10V──► regulators
```

The LTC2668 is a 16-channel, 16-bit ±10V SoftSpan DAC. Channels 0–3 drive the
regulators; the other 12 channels are spare and controllable via the raw
voltage command.

## 2. Repository layout

| Path | Purpose |
|---|---|
| `platformio.ini` | PlatformIO env `megaatmega2560` (AVR, Arduino framework, monitor 115200) |
| `lib/LTC2668/LTC2668.{h,cpp}` | Self-contained LTC2668 SPI driver (class `LTC2668`) |
| `lib/LTC2668/ltc2668.pdf` | LTC2668 datasheet |
| `lib/LTC2668/ltc2668EVM.pdf` | DC2025A demo-board manual (wiring/jumper reference) |
| `lib/PressureControl/PressureControl.{h,cpp}` | Pressure↔voltage mapping (class `PressureController`) |
| `src/main.cpp` | Firmware entry point + serial command protocol |
| `gui/pressure_gui.py` | tkinter GUI (pyserial) |
| `gui/requirements.txt` | `pyserial>=3.5` |
| `README.md` | User-facing build/wiring/usage guide |

## 3. Architecture (three layers)

### Layer 1 — LTC2668 driver (`lib/LTC2668/`)

The original Analog Devices Linduino driver was replaced with a
self-contained class because it depended on `Linduino.h`/`LT_SPI.h`
(QuikEval shield code) that is not in this project. Kept: ADI's command
codes, span codes, and code↔voltage math.

- SPI: **mode 0, MSB first, 1 MHz** (deliberately slow for jumper-wire
  robustness; the part supports 50 MHz — raise in `begin()` if wired on PCB).
- Frames are **32-bit**: 1 don't-care byte + command|address byte + 16-bit
  data. The extra byte makes the DAC echo the *previous* frame on SDO;
  `lastWriteVerified()` exposes that check (needs MISO wired).
- The driver **tracks each channel's span** internally so `setVoltage()`
  always converts with the correct min/max. `begin()` assumes power-on span
  is 0–5V (DC2025A default jumpers); `setSpanAll()` overwrites this at boot.
- Key API: `begin(cs, hz)`, `setSpan/setSpanAll`, `setCode`,
  `setVoltage(ch, v)` (returns the clamped/quantized value actually set),
  `setVoltageAll`, `powerDown/powerDownAll`, `muxEnable/muxDisable`.

### Layer 2 — Pressure mapping (`lib/PressureControl/`)

`PressureController` owns 4 `RegulatorConfig` entries
(`{name, dacChannel, vMin, vMax, pMin, pMax}`) defining a linear map

```
voltage = vMin + (p − pMin)·(vMax − vMin)/(pMax − pMin)
```

- Setpoints are clamped to the calibrated pressure range; vacuum is handled
  by `pMin > pMax`-style configs (clamping uses min/max of the two).
- `setCalibration()` validates (rejects degenerate ranges) and **re-applies
  the current setpoint** under the new calibration.
- `zeroAll()` drives every regulator to its zero-pressure voltage.

Defaults match the actual hardware (all kPa):
reg 0 `AIR` ch0 = SMC **ITV0030-3BL** (0–10V → +1…+500 kPa);
regs 1–3 `VAC1..3` ch1–3 = SMC **ITV2090-312L5** (0–10V → −1.3…−80 kPa).
Adjust in `PressureControl.h` (compile-time) or via `CAL` (runtime, not
persisted — lost on reset). Note "zero" for the vacuum regs clamps to
−1.3 kPa = 0V (minimum vacuum), and for air to +1 kPa = 0V.

### Layer 3 — Firmware protocol (`src/main.cpp`)

Boot sequence: `Serial.begin(115200)` → `dac.begin(53)` → all 16 channels to
**±10V span** → all outputs **0V** → regulators to zero-pressure → prints
`OK SRFM-DAC v1.0 ready`.

Line-based ASCII protocol, `\n` or `\r` terminated, one `OK …`/`ERR …` reply
per command:

| Command | Action |
|---|---|
| `ID` | → `OK SRFM-DAC v1.0` |
| `P <reg 0-3> <pressure>` | set regulator pressure → `OK AIR p=50.00 v=5.000` |
| `V <ch 0-15> <volts>` | raw DAC voltage (−10…+10) → `OK ch4 v=-2.500` |
| `CAL <reg> <vMin> <vMax> <pMin> <pMax>` | runtime calibration → `OK cal set` |
| `GET` | → `OK AIR=0.00,0.000V; VAC1=…; VAC2=…; VAC3=…` |
| `ZERO` | all 16 channels 0V, regulators to zero-pressure → `OK all zero` |

`DAC_CS_PIN = 53` (Mega hardware SS). Line buffer is 96 chars; overlong
lines are dropped with an error.

### GUI (`gui/pressure_gui.py`)

- tkinter + pyserial; background reader thread feeds a queue polled every
  50 ms on the Tk main loop (no cross-thread Tk calls).
- One panel per regulator: slider (command sent **on release only**, to
  avoid flooding serial), numeric entry + Set, readback label populated by
  parsing `GET` replies.
- After each `P` ack the GUI issues `GET` to refresh readbacks.
- Safety: `ZERO ALL` button; `ZERO` is also sent on window close.
- Connect waits **2.5 s** after opening the port (Mega auto-resets on open).
- The `REGULATORS` list at the top defines slider ranges — **must be kept in
  sync with the firmware calibration manually**; the GUI does not query
  calibration from the device.

## 4. Hardware setup (from the DC2025A manual in `lib/LTC2668/`)

| Mega | DC2025A | Note |
|---|---|---|
| 52 SCK | SCK | test points next to J1 (or J1 itself) |
| 51 MOSI | SDI | |
| 50 MISO | SDO | optional; enables write verification |
| 53 | CS/LD | |
| 5V | **OVP** | mandatory — powers EVM digital interface at Arduino logic level |
| GND | GND | |

- Bench supply **V+ = +15V, V− = −15V** on the turret posts (±15V required
  for the ±10V span).
- Jumpers, as configured on our board (= factory default):
  **JP1 REF_SEL = INT**, **JP2 MSP0 = 1**, **JP3 MSP1 = 1**, **JP4 MSP2 = 1**.
  MSP 1/1/1 = SoftSpan mode (power-up 0–5V span, zero-scale); the driver's
  `begin()` assumes exactly this power-on state. Other MSP settings force a
  fixed manual span and span commands are ignored — don't change them.
- Regulator command inputs on **VOUT0–VOUT3**.
- DC2025A-A = 16-bit part, DC2025A-B = 12-bit. Firmware assumes 16-bit
  codes; a -B board still works (low 4 bits ignored by the part).

## 5. Build / run

```sh
pio run                          # compile  (PlatformIO 6.1.19; CLI at ~/.platformio/penv/bin/pio)
pio run -t upload                # flash
pio device monitor               # expect "OK SRFM-DAC v1.0 ready"
pip install -r gui/requirements.txt
python3 gui/pressure_gui.py
```

Last verified build: SUCCESS — RAM 534 B (6.5%), flash 8804 B (3.5%).

## 6. Design decisions & gotchas

- **±10V span everywhere** even though regulators only need 0–10V: matches
  the stated requirement and leaves negative range for the 12 spare
  channels. Consequence: 0V = mid-scale code 0x8000, and effective
  resolution over 0–10V is 15 bits (~0.3 mV/LSB) — far beyond what a
  pressure regulator needs.
- **No EEPROM persistence** for `CAL` — calibration resets to compiled
  defaults on power-up. Add EEPROM storage if runtime cal must survive
  reboots.
- **No regulator feedback**: this is open-loop setpoint control. `GET`
  reports commanded values, not measured pressure. The LTC2668 MUX pin +
  driver `muxEnable()` could be used with a Mega ADC pin for DAC-output
  readback if needed.
- Firmware replies exactly one line per command — hosts can rely on
  send→read-line lockstep.
- The old Linduino `LTC2668_write/…_voltage_to_code` free functions are
  gone; anything copied from ADI example sketches must be ported to the
  class API.

## 7. Untested / next steps

1. **Hardware smoke test**: flash, wire per §4, `V 0 5` → measure 5.000V on
   VOUT0; `V 0 -10` → −10V; verify `ZERO`.
2. **Verify calibration against a gauge**: defaults use datasheet ranges for
   the SMC ITV0030-3BL (air) and ITV2090-312L5 (vacuum, both 0–10V command);
   trim with `CAL` / header edits if a reference gauge disagrees.
3. Regulators need their own 24 VDC supply (command signal is 0–10V from the
   DAC, but the ITV valves are 24V-powered devices).
4. Optional: EEPROM cal persistence, GUI cal editor, status polling timer.

Repo: https://github.com/YILINUMICH/SRFM (private).
