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
| `gui/pressure_gui.py` | tkinter GUI (needs `pip install -r requirements.txt`) |
| `gui/profiles/` | JSON test profiles for long unattended runs, with their own [README](gui/profiles/README.md) |

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

### Supply inputs

| EVM input | Voltage | Source | Note |
|---|---|---|---|
| V+ | +15V nominal | bench supply | turret post; supplies the output stage with headroom above the 10V full scale |
| V− | −15V nominal | bench supply | turret post; needed for bipolar spans, and fine to leave connected on the 0–10V span in use |
| OVP | 5V (1.71–5.5V) | Arduino Mega 5V pin | sets SPI logic level; SPI is dead without it |
| AVP | — | onboard LT1761-5 from V+ | no external connection needed |
| GND | 0V | common | bench supply, EVM, and Mega grounds tied together |

The regulators themselves need a separate **24 VDC** supply (the DAC only
provides their 0–10V command signal).

Jumper configuration as set on our board — note this is **not** SoftSpan:

| Jumper | Function | Setting |
|---|---|---|
| JP1 | REF_SEL | **INT** (internal 2.5V reference) |
| JP2 | MSP0 | 1 |
| JP3 | MSP1 | 1 |
| JP4 | MSP2 | **0** |

Only JP4 has been re-checked against the physical board; JP1–JP3 are as
originally recorded.

Because MSP2 = 0, the MSPx pins are **not** all 1, so the part is not in
SoftSpan mode — it is locked into a fixed manual span, and SPI span
commands are accepted but have no effect on the hardware.

The span actually in force was established by measurement rather than by
decoding the jumpers: with the firmware assuming ±10V, a commanded 5V
measured **7.5V** at VOUT0. That is code 49151 read across a 10V window,
which is only consistent with a **0–10V** span — every other span gives a
different voltage (0–5V → 3.75, ±5V → 2.50, ±2.5V → 1.25).

That span is the correct one for this system regardless: both regulator
models take a 0–10V command signal, so ±10V would have spent half the
code range on voltages they cannot use. The firmware programs
`LTC2668_SPAN_0_TO_10V` in `setup()`, which keeps the driver's code
conversion correct in either mode — ignored in fixed mode (where the span
already is 0–10V), applied in SoftSpan.

**Watch out for this:** in fixed mode the `SPAN` command still updates the
driver's conversion math while the hardware stays put, which reproduces
exactly the wrong-voltage bug above. `DUMP` reports the driver's span per
channel so a mismatch is visible. Setting JP4 to 1 puts the part in
SoftSpan and makes the firmware authoritative; nothing else changes,
since it already asks for 0–10V.

### DAC channel assignment

| DAC ch | EVM output | Regulator | Firmware name | 0–10V maps to |
|---|---|---|---|---|
| 0 | VOUT0 | SMC ITV0030-3BL (air pressure) | `AIR` | +1 … +500 kPa |
| 1 | VOUT1 | SMC ITV2090-312L5 (vacuum) | `VAC1` | −1.3 … −80 kPa |
| 2 | VOUT2 | SMC ITV2090-312L5 (vacuum) | `VAC2` | −1.3 … −80 kPa |
| 3 | VOUT3 | SMC ITV2090-312L5 (vacuum) | `VAC3` | −1.3 … −80 kPa |
| 4–15 | VOUT4–15 | spare | — | raw 0–10V via `V <ch> <volts>` |

Regulator index in the `P` command equals the DAC channel number. The
mapping lives in the `RegulatorConfig` table in
`lib/PressureControl/PressureControl.h` (`dacChannel` field) — change it
there if the wiring changes.

## Deployment

From a fresh checkout to a running system. Wiring and supplies should
already be done — see the sections above.

**1. Install the tools.** Needs Python 3.9 or newer:

```sh
pip install -r requirements.txt     # pyserial, for the GUI
pip install platformio              # firmware toolchain
```

**2. Build and flash the firmware.** Plug the Mega in over USB first:

```sh
pio run                             # compile
pio run -t upload                   # flash
```

PlatformIO picks the port itself. If it picks the wrong one, name it —
`COM3` on Windows, `/dev/cu.usbmodem*` on macOS, `/dev/ttyACM*` on Linux:

```sh
pio run -t upload --upload-port COM3
```

**3. Confirm it came up:**

```sh
pio device monitor                  # 115200 baud
```

Expect `OK SRFM-DAC v1.0 ready`. Type `VERIFY` — a reply of `OK verify=yes`
means the SPI link to the DAC is good. Ctrl-C to quit the monitor, and make
sure it *is* closed before starting the GUI: only one program can hold the
port at a time.

**4. Run the GUI:**

```sh
python gui/pressure_gui.py
```

Pick the port, press Connect, and the four regulator panels go live.

> **No serial port listed?** Check the USB cable carries data — charge-only
> cables power the board and enumerate nothing, which looks identical to a
> dead Mega. Try a different cable and a direct port rather than a hub before
> suspecting the board.

## Serial protocol (115200, one command per line)

Pressure layer — normal operation:

| Command | Meaning | Reply |
|---|---|---|
| `ID` | identify | `OK SRFM-DAC v1.0` |
| `P <reg 0-3> <pressure>` | set regulator pressure (eng. units) | `OK AIR p=100.00 v=1.984` |
| `CAL <reg> <vMin> <vMax> <pMin> <pMax>` | set regulator calibration | `OK cal set` |
| `GET` | status of all 4 regulators | `OK AIR=0.00,0.000V; VAC1=…` |
| `ZERO` | everything to 0 / safe state | `OK all zero` |

Raw DAC layer — bring-up and debugging, bypasses the pressure mapping:

| Command | Meaning | Reply |
|---|---|---|
| `V <ch 0-15> <volts>` | set a channel to a voltage in its span | `OK ch0 v=5.000 code=32767` |
| `C <ch 0-15> <code 0-65535>` | write a raw 16-bit DAC code | `OK ch0 code=16384 v=2.500` |
| `SPAN <span>` | set span on all channels | `OK span=1 range=0.00..10.00V on all channels` |
| `SPAN <ch> <span>` | set span on one channel | as above, `on ch0` |
| `DUMP` | code, voltage and span for all 16 channels | `OK ch0=16384,2.500V,s1; …` |
| `VERIFY` | SDO readback health (needs MISO on pin 50) | `OK verify=yes` |

Span codes: `0`=0–5V, `1`=0–10V, `2`=±5V, `3`=±10V, `4`=±2.5V. See the
jumper section above — `SPAN` only reaches the hardware in SoftSpan mode.

Setpoints are clamped to the calibrated range; the reply always shows what
was actually applied. `GET` and `DUMP` report the DAC's own record of the
last code written, so a raw `V` or `C` on a regulator channel is reflected
honestly rather than hidden behind a stale pressure setpoint.

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
`REGULATORS` table at the top of `gui/pressure_gui.py` in sync, since the
GUI's ranges and preset voltages are computed from it.

## GUI

Started with `python gui/pressure_gui.py` — see [Deployment](#deployment).

Pick the Mega's port (`COM…` on Windows, `usbmodem…` on macOS), Connect
(waits ~2.5s for the auto-reset), then drive each regulator from its preset
buttons or its manual entries. Preset buttons are labelled with both the
pressure and the command voltage the firmware will program. Each panel also
takes a raw voltage, sending `V <ch> <volts>` for debugging.

`ZERO ALL` is the panic button; the GUI also sends `ZERO` on window close.

### Test profiles

**Script…** runs an unattended sequence of held setpoints from a JSON file,
for long-duration testing. Profiles live in `gui/profiles/`; two are
included — `quick_check.json` (~1 min, exercises both the pressure and raw
voltage paths) and `vacuum_soak.json` (~1h45m, 4 loops).

```json
{
  "name": "vacuum soak",
  "loops": 2,
  "zero_on_finish": true,
  "steps": [
    {"label": "pressurise", "hold_s": 30,  "set": [{"reg": 0, "kPa": 100}]},
    {"label": "soak",       "hold_s": 600, "set": [{"reg": 1, "kPa": -40}]},
    {"label": "hold as-is", "hold_s": 15,  "set": []},
    {"label": "spare ch",   "hold_s": 10,  "set": [{"ch": 4, "volts": 2.5}]}
  ]
}
```

A `set` entry is either `{"reg": 0-3, "kPa": <pressure>}` for the pressure
layer or `{"ch": 0-15, "volts": <volts>}` to drive a DAC channel directly.
An empty `set` holds whatever the previous step left in place.

The whole file is validated before the run starts — including every
setpoint against that regulator's calibrated range — so a typo is caught up
front rather than eight hours in. Manual controls lock while a profile runs;
**Stop** ends it within a quarter second and zeroes the outputs.

**See [`gui/profiles/README.md`](gui/profiles/README.md) for the full field
reference and worked examples.**
