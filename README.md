# SRFM Valve Controller — custom PCB

Custom 4-channel pneumatic valve controller: **Seeed XIAO nRF52840** +
**AD5724R** 4-channel 12-bit SPI DAC + **ADS1015** 4-channel I²C ADC +
**TPS26600** eFuse on the 24 V valve rail, driving **3 vacuum regulators +
1 air-pressure regulator**, with a Python GUI over USB serial.

This branch (`custom-pcb-nrf52840`) is the custom-PCB version. The Arduino
Mega 2560 + LTC2668 EVM version lives on `main` — see
[Differences from the Mega version](#differences-from-the-mega-version-main-branch).

```
┌────────┐ USB CDC  ┌───────────────┐ SPI mode 2 ┌──────────┐ 0..10.8 V, 100 Ω  ┌───────────────┐
│ Python │ ───────► │ XIAO nRF52840 │ ─────────► │ AD5724R  │ ────────────────► │ 4 SMC valves  │
│  GUI   │  115200  │  (firmware)   │            │ DAC ×4   │  command 0..10 V  │ 3 vac + 1 air │
└────────┘          │               │ I²C 0x48   ┌──────────┐  monitor 1..5 V   │               │
                    │               │ ◄───────── │ ADS1015  │ ◄──────────────── │               │
                    │               │ SHDN / FLT ┌──────────┐  24 V rail        │               │
                    │               │ ─────────► │ TPS26600 │ ────────────────► │               │
                    └───────────────┘            └──────────┘                   └───────────────┘
```

Two documents are authoritative and this README only summarises them:

- [`FIRMWARE_HANDOFF.md`](FIRMWARE_HANDOFF.md) — hardware → firmware handoff:
  pin map, register sequences, boot order, safety invariants, bring-up.
- [`PROTOCOL.md`](PROTOCOL.md) — the host command set.

## Project layout

| Path | What it is |
|---|---|
| `platformio.ini` | PlatformIO config, env `xiao_nrf52840` (nordicnrf52, Adafruit nRF52 core) |
| `boards/xiao_nrf52840.json` | Board definition — PlatformIO has no stock XIAO nRF52840 |
| `variants/Seeed_XIAO_nRF52840/` | Pin-map variant, copied from Seeed's Arduino core |
| `linker/nrf52840_s140_v7.ld` | Linker script placing the application at `0x27000` (SoftDevice S140 7.x) |
| `tools/upload_dfu.py` | PlatformIO pre-upload hook: finds the board by USB VID, sends `DFU`, waits for the bootloader port (see [Deployment](#deployment)) |
| `SRFMV1/` | Board fab files and the netlist `SRFMV1.tel` the channel map was read from |
| `lib/AD5724R/` | AD5724R SPI DAC driver (24-bit frames, SPI mode 2, register readback) |
| `lib/ADS1015/` | ADS1015 I²C ADC driver (single-shot, averaging) |
| `lib/ValveConfig/` | Persisted calibration/config struct (channel map, full-scale codes, readback gain/enable, pressure endpoints, heartbeat timeout) — version + CRC in LittleFS `/srfm_cal.bin` |
| `lib/PressureControl/` | Pressure ↔ valve-voltage linear mapping |
| `src/main.cpp` | Firmware: boot sequence, watchdog, heartbeat, fault handling, host protocol |
| `gui/pressure_gui.py` | tkinter GUI (needs `pip install -r requirements.txt`) |
| `gui/profiles/` | JSON test profiles for long unattended runs, with their own [README](gui/profiles/README.md) |
| `FIRMWARE_HANDOFF.md` | Hardware → firmware handoff (authoritative for pins, registers, safety) |
| `PROTOCOL.md` | Host protocol (authoritative for commands and replies) |
| `HANDOFF.md` | Project handoff: architecture, design decisions, open items |

## Hardware

Everything on one board. The XIAO is soldered down and **SWD is not
accessible** — the stock UF2/DFU bootloader is the only way in, so never
ship firmware that disables USB. The module on the assembled SRFMV1 board is
the XIAO nRF52840 **Sense** variant (USB VID 0x2886; PID 0x8045 with the
stock firmware, 0x8044 with ours, 0x0045 in the bootloader); its IMU is not
on D4/D5, so there is no I²C conflict.

### Pin map (as built — FIRMWARE_HANDOFF §1)

| XIAO pad | Port | Signal | Board pull | Note |
|---|---|---|---|---|
| D0 | P0.02 | spare | — | reserved for the pneumatic shut-off solenoid |
| D1 | P0.03 | spare | — | reserved for eFuse IMON (DNP) |
| D2 | P0.28 | DAC **SYNC** (CS, active low) | 10 kΩ pull-up | idle high |
| D3 | P0.29 | DAC **CLR** (active low) | 10 kΩ pull-**down** | **low = DAC at 0 V and all SPI writes ignored** |
| D4 | P0.04 | I²C **SDA** | 4.7 kΩ pull-up | BSP default `Wire` |
| D5 | P0.05 | I²C **SCL** | 4.7 kΩ pull-up | BSP default `Wire` |
| D6 | P1.11 | eFuse **SHDN** | 10 kΩ pull-**down** | **high = 24 V rail on**; MainPower switch grounds it |
| D7 | P1.12 | eFuse **FLT** (active low) | 100 kΩ pull-up | low = fault latched |
| D8 | P1.13 | SPI **SCK** | 33 Ω series | BSP default `SPI` |
| D9 | P1.14 | SPI **MISO** ← DAC SDO | 33 Ω series | enables register readback |
| D10 | P1.15 | SPI **MOSI** → DAC SDIN | 33 Ω series | |

The pulls define the **hardware safe state**: whenever the MCU's pins are
high-Z (before boot, during reset, after a watchdog reset) CLR is asserted
(DAC 0 V) and SHDN is low (24 V off). Firmware only ever *leaves* that state,
deliberately and in order. Full detail, bus settings and register values:
[`FIRMWARE_HANDOFF.md`](FIRMWARE_HANDOFF.md) §1–§4.

### Channels

Channels are **1-indexed CH1–CH4** in every host message, matching the
silkscreen.

| CH | Valve | Firmware name | 0–10 V at the valve maps to | DAC output (series R) | ADC input |
|---|---|---|---|---|---|
| 1 | SMC ITV2090-312L5 (vacuum) | `VAC1` | −1.3 … −80 kPa | VOUT **C** (R18) | AIN **3** |
| 2 | SMC ITV2090-312L5 (vacuum) | `VAC2` | −1.3 … −80 kPa | VOUT **D** (R19) | AIN **2** |
| 3 | SMC ITV2090-312L5 (vacuum) | `VAC3` | −1.3 … −80 kPa | VOUT **B** (R20) | AIN **1** |
| 4 | SMC ITV0030-3BL (air pressure) | `AIR` | +1 … +500 kPa | VOUT **A** (R21) | AIN **0** |

> **The physical map above is verified.** It comes from the SRFMV1 netlist
> (`SRFMV1/SRFMV1.tel`: VOUTC→R18→CMD1, VOUTD→R19→CMD2, VOUTB→R20→CMD3,
> VOUTA→R21→CMD4; AIN3…AIN0 ← RD1…RD4) and was confirmed on the bench on
> 2026-09-10 (VOUTB measured on the CH3 pads). It is the firmware default
> (`map=CDBA/3210`). The Gerber-review prediction in FIRMWARE_HANDOFF §5
> (`BACD`) was wrong on the DAC side.

CH4's ITV0030-3BL may have no monitor output at all. Its readback can be
disabled (`CAL RBEN 4 0`), in which case it reports `n/a` and never raises an
open-load fault.

## Deployment

From a fresh checkout to a running system.

**1. Install the tools.** Needs Python 3.9 or newer:

```sh
pip install -r requirements.txt     # pyserial, for the GUI
pip install platformio              # firmware toolchain
```

**2. Build and flash.** Plug the XIAO in over USB first:

```sh
pio run                             # compile
pio run -t upload                   # flash via the stock bootloader (serial DFU)
```

The upload uses `adafruit-nrfutil` serial DFU through the XIAO's stock
bootloader; PlatformIO fetches the tool itself. PlatformIO's own 1200-baud
touch is **not** used (`board_upload.use_1200bps_touch = no`). Instead the
pre-upload hook `tools/upload_dfu.py` finds the board by USB VID 0x2886: if a
bootloader port (PID 0x0044/0x0045) is already there it uses it; otherwise it
opens the application port (PID 0x8044/0x8045), sends the `DFU` command,
waits up to 15 s for the bootloader port to appear and hands that port to
`adafruit-nrfutil`. `--upload-port` is optional and only forces which
application port gets the `DFU`:

```sh
pio run -t upload --upload-port COM3
```

Why the detour: the nRF52 watchdog keeps running across a soft reset and the
XIAO's stock bootloader (Adafruit-derived UF2 bootloader 0.6.1, S140 7.3.0)
does not feed it, so the standard touch → soft reset → DFU gets cut off ~2 s
in and leaves the bootloader in DFU mode with an invalid app (a second upload
then works, but that is the recovery, not the flow). The firmware's `DFU`
entry is therefore a double hop: outputs and rail to the safe state, write a
request file `/dfu_req` in internal LittleFS, stop kicking the watchdog →
watchdog reset (the only reset that stops the WDT) → the freshly booted
firmware sees the file before starting the watchdog, deletes it and performs
the core's normal soft reset into serial DFU. The core's `enterSerialDfu()`
is redirected to this path with `-Wl,--wrap=enterSerialDfu`, so a 1200-baud
touch from other tools (Arduino IDE) also works, just slower (~3 s). Verified
on the bench 2026-09-10; see [`HANDOFF.md`](HANDOFF.md) § Bench status.

The toolchain is not stock: the repo carries the board JSON, the pin variant
and a linker script because PlatformIO does not know the XIAO nRF52840 and
its core package only ships SoftDevice 6.1.1 headers, while the XIAO's
bootloader carries S140 7.3.0. The 6.1.1 headers are API-compatible for the
SoC calls this firmware makes; the linker script puts the application at
`0x27000` where the 7.x SoftDevice expects it. See [`HANDOFF.md`](HANDOFF.md)
§ Toolchain.

**Fallback — UF2 via the bootloader.** If serial DFU fails or the firmware
hangs so the `DFU` request cannot reach it (a build that is stuck before it
services the serial port, for instance), **double-tap the XIAO's reset
button**. A USB drive named `XIAO-SENSE` appears and the bootloader port
enumerates, so `pio run -t upload` works again as-is (the hook sees the
bootloader port and skips the `DFU` step). Alternatively convert the build to
UF2 with `uf2conv.py` from [Microsoft's uf2
repo](https://github.com/microsoft/uf2) and copy it onto the drive:

```sh
python uf2conv.py -f 0xADA52840 -b 0x27000 -c -o firmware.uf2 .pio/build/xiao_nrf52840/firmware.hex
```

The build also leaves `.pio/build/xiao_nrf52840/firmware.zip`, the DFU
package `adafruit-nrfutil` uploads. With no SWD on the board, the bootloader
is the **only** recovery path — but a hung firmware is not dangerous: the
watchdog resets the MCU within ~2 s and the board pulls return the DAC to
0 V and drop the 24 V rail while the pins are high-Z.

**3. Confirm it came up:**

```sh
pio device monitor                  # 115200 (CDC ignores the baud)
```

The boot-time `!READY` / `!FAULT` line is printed before USB is up, so you
will not see it; `STATUS` shows the same information (`state=READY`,
`dac=0x001F`), and `START` prints a fresh `!READY ch1=… ch4=…`. Type `ID` →
`OK SRFM-PCB v2.0 state=READY map=CDBA/3210 cal=ok hb=2 rst=0x0
bootword=0xFFFFFFFF` (the trailing `rst=`/`bootword=` are boot diagnostics:
`RESETREAS` and the raw `.noinit` request word as seen at boot — the
bootloader clears `RESETREAS` before starting the app, so `rst=0x0` is
normal). Type `VERIFY` → `OK verify=yes pc=0x001F` proves the SPI link to
the DAC. Ctrl-C to quit, and make sure the monitor *is* closed before starting
the GUI: only one program can hold the port.

> Typing by hand? The link-loss heartbeat defaults to **2 s** and arms on the
> first command, so a slow second command trips `!FAULT LINKLOST`. Send
> `HBT 0` first when driving the board from a terminal (`START` recovers from
> LINKLOST). `CAL DEFAULT` + `CAL SAVE` puts the saved timeout back to 2 s.
>
> With the 24 V supply absent the board boots into `!FAULT FLT`: the eFuse
> is unpowered and its FLT pin reads low. That is expected; `CLEARFAULT` (or
> `START`) recovers once 24 V is present.

**4. Run the GUI:**

```sh
python gui/pressure_gui.py
```

Pick the port, press Connect, and the four valve panels go live.

> **No serial port listed?** Check the USB cable carries data — charge-only
> cables power the board and enumerate nothing. Try a different cable and a
> direct port rather than a hub before suspecting the board.

## Safety

The rules the firmware implements (FIRMWARE_HANDOFF §6–§7). Read them before
changing `src/main.cpp`.

**Boot order** — leave the hardware safe state in exactly this sequence:
GPIOs to the pulled state (SYNC high, CLR low, SHDN low) → SPI/I²C init →
**watchdog on** → CLR high → DAC power control `0x001F` → output range +10.8 V
→ control (clamp + TSD enable) → code 0 on all four → **read back power
control; if it is not `0x001F` stop here and never enable the rail** → ADS1015
ACK check → SHDN high (rail ramps ~90 ms) → wait ~150 ms, read FLT, sweep the
monitors → `!READY`.

**Stop order** — `STOP`, heartbeat loss and eFuse faults all do: **code 0 on
every channel first, then SHDN low.** A valve vents on 0 V; cutting 24 V
first leaves its output pressure "retained temporarily and not guaranteed".

**Heartbeat** — USB CDC gives no reliable disconnect signal, so if no valid
command arrives within the timeout (default 2 s, `HBT <s>`, `0` = off for the
bench) the board runs the stop sequence and reports `!FAULT LINKLOST`. The
GUI sends `HB` every second automatically.

**Watchdog** — 2 s, kicked only from the main loop after a good housekeeping
pass. A WDT reset returns every pin to high-Z, and the pulls do the rest.

**eFuse fault** — `FLT` low means the TPS26600 has latched off (over-current
≥ 0.99 A, UVLO, OVP, reverse, thermal — firmware can't tell which). The rail
never comes back by itself; only an explicit `CLEARFAULT` cycles SHDN.

**What the board does NOT do:**

- **Electrical off is not pneumatic off.** An unpowered ITV passes supply
  pressure straight through. The pneumatic shut-off solenoid is not on this
  board revision. **Do not commission with air connected until it exists.**
  It is intended for a spare GPIO (D0/D1) driving a low-side FET with a gate
  pull-down, so a hang or reset closes the air.
- The monitor readback is a ±6 % F.S. liveness signal, not feedback. Nothing
  closes a loop on it.
- Out-of-range host commands are rejected with `ERR`, never clamped (except
  `P`, which clamps to the calibrated pressure range like the Mega firmware
  and reports what it applied).

## Bench bring-up

FIRMWARE_HANDOFF §9, with the exact commands. **No air connected. MainPower
switch OFF until step 4.** Open `pio device monitor` and send `HBT 0` first
so the heartbeat does not interrupt you.

> **Where it stands (2026-09-10):** steps 1–3 pass. Step 2 is blocked by a
> board fault — every DAC output hits its 20 mA clamp at ~0.67 V because the
> TVS diodes D7–D10 on VOUTA–D are fitted forward-biased to ground. Fix them
> before continuing (details in [`HANDOFF.md`](HANDOFF.md) § Bench status).

**1. SPI proof.** `STATUS` should show `state=READY` (or `state=FAULT
reason=SPI` when the DAC readback failed, in which case the rail stays off).
Then:

```
VERIFY                 → OK verify=yes pc=0x001F
```

If `verify=no`: check CLR is actually high on D3 (low = every write is
ignored), then `SPIMODE 1` and `VERIFY` again, then scope SYNC/SCK/MOSI.
`CAL SAVE` persists the mode that works, so the next boot starts in it.
*Bench 2026-09-10: PASS in mode 2; `RAWGET` readbacks return what was
written.*

**2. DAC channel map.** With SHDN still low (AVDD is up regardless), write a
half-scale code to one *physical* output at a time and DMM every valve pad
field's pin 2 (white):

```
RAW A 2048             → OK dacA code=2048 vdac=5.400
RAWGET A               → OK dacA code=2048 range=2 func=0xC vdac=5.400
```

`RAWGET` reads the DAC's own registers back, so it separates "the write did
not land" from "the output stage is not delivering". Whichever pad reads
~5.3 V is wired to VOUTA — per the netlist that is the **CH4** pads
(VOUTA→R21→CMD4). Repeat `RAW B 2048`, `RAW C 2048`, `RAW D 2048` (writing
`RAW X 0` in between) and note the pad for each letter. A result different
from the [channel table](#channels) is not a firmware problem — record what
you measured. *Bench 2026-09-10: VOUTB seen on the CH3 pads, confirming the
netlist; but the pads only track the DAC below ~0.67 V and `!FAULT OC n`
fires above — see the D7–D10 note above.*

**3. I²C proof.** The ADS1015 must ACK at 0x48 and read near zero with
nothing driving its inputs:

```
ADC 0                  → OK ain0 code=<small> v=<~0>
```

*Bench 2026-09-10: PASS; ACK at 0x48, inputs read ~0.02 V with nothing
connected.*

**4. Rail and readback map.** MainPower ON, then:

```
RAIL ON                → OK rail=on
```

Confirm the `FuseGood` LED. Connect one valve to the **CH1** pad field only
and read all four inputs:

```
ADC 0
ADC 1
ADC 2
ADC 3
```

The input that rises to ~1 V (code ≈ 314, 0 % F.S.) is wired to CH1 — expect
AIN3 (netlist: AIN3/2/1/0 ← RD1/2/3/4; not yet confirmed with a valve). Repeat
for the CH2, CH3 and CH4 pad fields. If the measured map differs from the
default `CDBA/3210`, enter it (DAC letter from step 2, ADC input from this
step) and save it — `CAL MAP` swaps DAC outputs between channels when the
requested output is already held by another channel, so any permutation can
be entered line by line:

```
CAL MAP 1 C 3
CAL MAP 2 D 2
CAL MAP 3 B 1
CAL MAP 4 A 0
CAL RBEN 4 0           ← only if the CH4 ITV0030 has no monitor and stays near 0
CAL SAVE               → OK saved
ID                     → … map=CDBA/3210 cal=ok …
```

**5. Loopback sanity.** Get the board into `READY` (`STATUS` shows the state;
`START` if it is `STOPPED`), then command 50 % on a channel and read its
monitor:

```
SET 1 50               → OK ch1 pct=50.0 code=1926
GET 1                  → OK ch1 cmd=50.0 mon=<≈50> status=ok
```

The monitor should read 50 % ± 6 %. Repeat on each channel.

**6. Cross-coupling test.** Step one channel 0 → 100 % while logging all four
monitors; the other three must not move:

```
SETALL 0 0 0 0
STATUS
SET 2 100
STATUS                 (repeat a few times over ~2 s)
```

`STATUS` prints `ch1=<cmd>,<mon>,<status> … ch4=…` on one line, which is easy
to log from the terminal. This is the test that decides whether the isolated
DC-DC footprints get populated.

**7. Watchdog test.**

```
HANG                   → OK hanging
```

The main loop stalls. Within ~2 s the rail must drop (`FuseGood` off), the
DAC must clear to 0 V, and the board must re-enumerate on USB and print a new
`!READY`. Reconnect the monitor afterwards.

## Host protocol (115200, one reply line per command)

Summary only — replies, edge cases and the full bench command set are in
[`PROTOCOL.md`](PROTOCOL.md). Commands are case-insensitive; every command
gets exactly one `OK …` / `ERR …` / `FAULT …` line, and asynchronous events
arrive as extra `!…` lines.

Operating:

| Command | Meaning |
|---|---|
| `ID` | `OK SRFM-PCB v2.0 state=… map=… cal=… hb=… rst=0x… bootword=0x…` |
| `SET <ch> <pct>` / `SETALL <p1> <p2> <p3> <p4>` | percent of full scale, 0–100, out of range → `ERR` |
| `P <ch> <kPa>` | pressure in engineering units through the calibration (clamped, reply shows what was applied) |
| `V <ch> <volts>` / `C <ch> <code>` | voltage at the valve (0–10) / raw DAC code (0–`code_full_scale`) |
| `GET` / `GET <ch>` | all channels (pressure, volts, monitor) / one channel with status |
| `STATUS` | one line: state, rail, FLT, DAC power-control readback, cal, hb, per-channel cmd/mon/status |
| `ZERO` | every channel to 0 %, rail stays on |
| `STOP` / `START` | codes 0 then rail off / re-run boot steps 8–12 |
| `CLEARFAULT` | only path that cycles SHDN after an eFuse latch |
| `HB` / `HBT <s>` | heartbeat / link-loss timeout (0 = off) |
| `CAL …` | calibration, below |

Bench: `VERIFY`, `SPIMODE`, `RAW`, `RAWGET`, `ADC`, `RAIL`, `DUMP`, `HANG` —
see [Bench bring-up](#bench-bring-up). `DFU` (safe state, then into the
bootloader — what `pio run -t upload` sends) and `RAMTEST <SOFT|WDT>`
(reset diagnostic) are in `PROTOCOL.md` too.

Events: `!READY …`, `!FAULT FLT`, `!FAULT LINKLOST`, `!FAULT OPENLOAD <ch>`,
`!FAULT NORAIL`, `!FAULT STUCK <ch>`, `!FAULT OC <ch>`, `!FAULT TSD`.
Per-channel faults are reported, not acted on; FLT and LINKLOST run the stop
sequence.

## Calibration

Everything that depends on the board in hand is data, persisted in internal
flash (LittleFS `/srfm_cal.bin`, versioned with a CRC), not code:

| Term | `CAL` command | Default | What it corrects |
|---|---|---|---|
| Channel map | `CAL MAP <ch> <A-D> <0-3>` | `CDBA/3210` | which DAC output / ADC input serves each logical channel |
| Full-scale code | `CAL FS <ch> <code>` | 3851 (CH1–3), 3831 (CH4) | DAC code that puts 10.000 V at the valve through the 100 Ω series resistor. Measure at valve pin 2 with a DMM; never commanded above |
| Readback gain/offset | `CAL RB <ch> <mV_per_code> <offset>` | 3.19 mV, 0 | monitor divider + ADC input impedance. Fit against a DMM at valve pin 4 at ~50 % and ~100 % |
| Readback enable | `CAL RBEN <ch> <0\|1>` | 1 | 0 → monitor reported as `n/a`, no open-load fault |
| Pressure endpoints | `CAL PRESS <ch> <vMin> <vMax> <pMin> <pMax>` | datasheet ranges (table above) | pressure ↔ valve-voltage line used by `P` and the GUI |
| Heartbeat timeout | `HBT <s>` | 2 | saved with the rest |

`CAL GET` prints the lot; `CAL SAVE` writes it; `CAL DEFAULT` reloads the
nominal values into RAM. **`cal=uncal`** in `ID`/`STATUS` means flash held no
valid record (missing, wrong version, or bad CRC) and the nominal values are
in use — fine for the bench, not for a commissioned rig.

Keep the `REGULATORS` table at the top of `gui/pressure_gui.py` in sync with
the pressure endpoints; the GUI computes its ranges and presets from it and
does not query calibration from the device.

## GUI

Started with `python gui/pressure_gui.py` — see [Deployment](#deployment).

Pick the port, Connect, and drive each valve from its preset buttons or its
manual entries. Preset buttons are labelled with both the pressure and the
command voltage the firmware will program. Each panel also takes a raw
voltage, sending `V <ch> <volts>` for debugging, and shows the valve's
**monitor readback** (percent of full scale, or `n/a` for a channel with
readback disabled) alongside the commanded value.

The GUI sends `HB` every second automatically, so the link-loss heartbeat
stays armed at its default; unplugging the cable or killing the GUI zeroes
the outputs within ~2 s. `CLEAR` drops one valve back to zero, `ZERO ALL` is
the panic button (rail stays on), and `ZERO` is sent on window close.

### Test profiles

**Script…** runs an unattended sequence of held setpoints from a JSON file,
for long-duration testing. Profiles live in `gui/profiles/`.

```json
{
  "name": "vacuum soak",
  "loops": 2,
  "zero_on_finish": true,
  "steps": [
    {"label": "pressurise", "hold_s": 30,  "set": [{"reg": 4, "kPa": 100}]},
    {"label": "soak",       "hold_s": 600, "set": [{"reg": 1, "kPa": -40}]},
    {"label": "hold as-is", "hold_s": 15,  "set": []},
    {"label": "raw volts",  "hold_s": 10,  "set": [{"ch": 2, "volts": 2.5}]}
  ]
}
```

A `set` entry is either `{"reg": 1-4, "kPa": <pressure>}` for the pressure
layer or `{"ch": 1-4, "volts": <0..10>}` to command a valve voltage directly.
Channel numbers are **1–4** as everywhere else on this branch (`main` used
0–3 and had spare channels 4–15; there are none here). The whole file is
validated before the run starts; manual controls lock while it runs;
**Stop** ends it within a quarter second and zeroes the outputs.

**See [`gui/profiles/README.md`](gui/profiles/README.md) for the full field
reference.**

## Differences from the Mega version (`main` branch)

| | `main` (Mega 2560 + DC2025A) | this branch (custom PCB) |
|---|---|---|
| MCU / toolchain | AVR, stock `megaatmega2560` board | nRF52840, custom board JSON + variant + linker script |
| DAC | LTC2668, 16 ch × 16-bit, ±10 V span, 32-bit frames, SPI mode 0 | AD5724R, 4 ch × 12-bit, +10.8 V range, 24-bit frames, SPI mode 2 |
| Channel numbering | 0–3 (+ 12 spares) | **1–4**, no spares; physical map is data (`CAL MAP`) |
| Full scale | 10 V at the DAC pin | `code_full_scale` per channel puts 10.000 V *at the valve* behind the 100 Ω series R |
| Valve feedback | none | ADS1015 monitor readback per channel, open-load / stuck / no-rail detection |
| Valve power | external 24 V, uncontrolled | on-board TPS26600 eFuse: rail switched by firmware, latching fault, `CLEARFAULT` |
| Safe state | software only | hardware pulls (CLR, SHDN) + watchdog + boot/stop ordering |
| Host link loss | ignored | heartbeat timeout → stop sequence |
| Calibration | compile-time defaults, `CAL` lost on reset | persisted in flash with version + CRC; `uncal` flag |
| Protocol | `OK SRFM-DAC v1.0`, `SPAN`, `DUMP` for 16 channels | `OK SRFM-PCB v2.0`, `SET`/`SETALL`/`STATUS`/`STOP`/`START`/`HB`, bench commands, `!` events |
| Flashing | avrdude | serial DFU via bootloader (`DFU` command + watchdog double hop, `tools/upload_dfu.py`), UF2 fallback; no SWD |
| Board state | always live | `READY` / `STOPPED` / `FAULT` gate every output command |
