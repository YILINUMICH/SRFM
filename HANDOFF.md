# HANDOFF — SRFM Valve Controller, custom PCB branch

**Date:** 2026-09-10
**Branch:** `custom-pcb-nrf52840` (the Mega 2560 + LTC2668 EVM version stays on `main`)
**Status:** Firmware runs on the assembled SRFMV1 board. Upload flow, SPI and I²C proven;
**bring-up stalled at step 2 by a board fault (TVS diodes D7–D10 on the DAC outputs)** — see
§0 below, then README § Bench bring-up / FIRMWARE_HANDOFF §9.

Companion documents, both authoritative over this one where they overlap:

- `FIRMWARE_HANDOFF.md` — what the hardware needs from firmware: pin map, register
  sequences, boot order, fault handling, calibration, bring-up. Its §0 lists three things
  that override the older design docs (stale pin table, unmeasured channel map, CH4 monitor
  possibly absent).
- `PROTOCOL.md` — the host command set, replies and events.

## 0. Bench status 2026-09-10

Assembled SRFMV1 board, XIAO nRF52840 **Sense** (USB VID 0x2886; PID 0x8045 stock firmware,
0x8044 with ours, 0x0045 in the bootloader). No air, no valves connected.

| Item | Result |
|---|---|
| **Upload flow** (`pio run -t upload`) | **Works.** `tools/upload_dfu.py` sends `DFU`, the firmware takes the watchdog double hop (~3 s), `adafruit-nrfutil` flashes through the bootloader port. See §5 for the mechanism and why the stock 1200-baud touch is not used. Fallback double-tap reset → `XIAO-SENSE` → `pio run -t upload` also works |
| Measured, upload-related | The bootloader clears `NRF_POWER->RESETREAS` before starting the app (`ID` shows `rst=0x0`). A `.noinit` RAM word does **not** survive watchdog reset + bootloader pass (`RAMTEST WDT` → `bootword=0xFFFFFFFF`), so the DFU request has to live in flash (`/dfu_req`) |
| Step 1 — SPI proof | **PASS** in SPI mode 2: `VERIFY` → `pc=0x001F`; `RAWGET` register readbacks return what was written |
| Step 3 — I²C proof | **PASS**: ADS1015 ACKs at 0x48, inputs read ~0.02 V with nothing connected |
| Channel map | From the SRFMV1 netlist (`SRFMV1/SRFMV1.tel`, now in the repo): VOUTC→R18→CMD1, VOUTD→R19→CMD2, VOUTB→R20→CMD3, VOUTA→R21→CMD4; AIN3/2/1/0 ← RD1/2/3/4. DAC side confirmed with a DMM (VOUTB seen on the CH3 pads). Firmware default and the saved config are now **`CDBA/3210`**. FIRMWARE_HANDOFF §5's `BACD` guess was wrong on the DAC side. ADC side still to be confirmed with a valve on each pad (step 4) |
| **Hardware fault — DAC outputs clamped** | Every DAC output hits its 20 mA current clamp at ~0.67 V with nothing connected (`!FAULT OC n`, power-control readback OC bit set). The netlist shows TVS diodes **D7–D10** (SMA footprint, same as D2 on the 24 V rail) with pin 1 on GND and pin 2 on VOUTA–D, i.e. **forward-biased from the DAC output to ground**; D2 has pin 1 on +24V (the correct TVS orientation). Measurements on U3: pin 24 AVDD 14.69 V, pin 1 AVSS 0 V, pin 14 DVCC 3.3 V, pin 17 REFOUT 2.5 V, pin 4 VOUTB 0.7 V when commanded 5.08 V; the pad tracks the DAC exactly below the clamp (0.527 V commanded → 0.532 V measured). **Fix pending:** rotate D7–D10 180° (only if their standoff is ≥ 11 V), or replace with a 12–15 V unidirectional SMA TVS in the correct orientation, or remove them for bring-up. Part number of D7–D10 unknown (EasyEDA cloud library only) |
| `!FAULT FLT` at boot with 24 V absent | Expected: FLT reads low while the eFuse is unpowered. `CLEARFAULT` / `START` recovers once 24 V is present |
| XIAO variant | **Sense** (PID 0x8045 stock). Closes the plain-vs-Sense open item; the IMU is not on D4/D5, no I²C conflict |
| Bench tip | Send `HBT 0` first when driving the board from a terminal; `CAL DEFAULT` + `CAL SAVE` resets the saved heartbeat to 2 s |

### CH3 after the D9 rework (2026-09-10, later)

D9 reworked on CH3 only. DAC output B then sweeps 0 to 10.15 V with the clamp
bit never set, and the CMD3 pad tracks the DAC pin with a pure gain error of
about +0.6 % (2.001 V → 2.016, 4.999 → 5.03, 8.000 → 8.05, 10.001 → 10.06),
no offset. That is the AD5724R internal-reference tolerance and is absorbed by
`CAL FS` once measured with the valve attached (nominal 3851 would become
about 3828 for CH3). Outputs A, C and D still clamp at 0.7 V until D10, D7
and D8 are reworked the same way. A transient 0.02 V reading on U3 pin 4
during this work was a probing artefact, not a dead output.

### ADC side of the channel map verified (2026-09-10, later)

With CMD3 driven to 3.0 V and a jumper from the CMD3 pad to each RD pad in
turn, exactly one ADS1015 input rose each time (about 0.977 V at the pin,
2 % above the 0.955 V expected through the 0.3139 divider): RD1→AIN3,
RD2→AIN2, RD3→AIN1, RD4→AIN0. Both halves of `CDBA/3210` are now measured.
The fault logic behaved: the jumpered channel reported `stuck` (monitor 53 %
against its command) and the others `open` with the rail on and no valve.

Also found during this session: after the D10 rework the DAC went silent
(readback 0x0000 / floating, REFOUT 0 V, DVCC read 0 V at pin 14 at one
point); the cause was a loose solder joint on the SCLK line, reflowed.
While pins were being pressed the eFuse latched once; `RAIL FORCE` now
cycles SHDN low first, which clears the latch. AVDD is regulated by U2 from
the switched +24V net (not from 3V3 as FIRMWARE_HANDOFF states), so the DAC
cannot drive any output while the rail is off.

### All four command paths clean (2026-09-10, late)

D7, D8, D9 and D10 all reworked. Every DAC output (A-D) sweeps 0 to 10.15 V
with the overcurrent bit never set. Bring-up steps 1-4 are complete on the
assembled board: SPI proof, DAC channel map (CDBA), I2C proof, ADC channel
map (3210). Step 7 (watchdog) passed: after `HANG` the USB port dropped at 1.15 s and
the board was back, re-enumerated and READY with the rail up, at 1.98 s.
Remaining: step 5 loopback with real valves, step 6 cross-coupling, then
`CAL FS` / `CAL RB` per channel.

## 1. What this project is

A control chain for **3 vacuum regulators + 1 air-pressure regulator** from a PC:

```
Python GUI ──USB CDC──► XIAO nRF52840 ──SPI──► AD5724R ──0..10 V──► 4 SMC ITV valves
                              │ ◄────I²C──── ADS1015 ◄──monitor 1..5 V──┘
                              └──SHDN/FLT──► TPS26600 eFuse ──24 V rail──► valves
```

Everything is on one custom PCB. The firmware drives the valve command voltages, reads the
valves' analog monitor outputs, switches and supervises the 24 V rail, and holds the whole
thing in a hardware-guaranteed safe state whenever it is not deliberately running.

## 2. Repository layout

| Path | Purpose |
|---|---|
| `platformio.ini` | env `xiao_nrf52840`: `nordicnrf52@~10.9.0`, Arduino framework (Adafruit nRF52 core, `framework-arduinoadafruitnrf52` 1.6.1), `upload_protocol = nrfutil`, monitor 115200; `build_flags = -Wl,--wrap=enterSerialDfu`, `extra_scripts = tools/upload_dfu.py`, `board_upload.use_1200bps_touch = no`, `board_upload.wait_for_upload_port = no` (see §5) |
| `tools/upload_dfu.py` | pre-upload hook: finds the board by USB VID 0x2886, uses a bootloader port if present, otherwise sends `DFU` to the application port and waits ≤ 15 s for the bootloader port |
| `boards/xiao_nrf52840.json` | board definition (see §5) |
| `SRFMV1/` | board fab outputs and the netlist `SRFMV1.tel` (source of the channel map and the D7–D10 finding) |
| `variants/Seeed_XIAO_nRF52840/variant.{h,cpp}` | pin map, copied from Seeed's Arduino core |
| `linker/nrf52840_s140_v7.ld` | application at `0x27000`, RAM from `0x20006000` (see §5) |
| `lib/AD5724R/` | DAC driver |
| `lib/ADS1015/` | ADC driver |
| `lib/ValveConfig/` | persisted calibration/config struct |
| `lib/PressureControl/` | pressure ↔ valve-voltage mapping (carried over from `main`, re-indexed) |
| `lib/ITV0030-3BL.pdf`, `lib/ITV2090-312L5.PDF` | valve datasheets |
| `src/main.cpp` | boot sequence, watchdog, heartbeat, fault handling, protocol |
| `gui/pressure_gui.py`, `gui/profiles/` | tkinter GUI and JSON test profiles |
| `requirements.txt` | `pyserial>=3.5` |
| `README.md` | user-facing: hardware summary, deployment, bring-up, protocol summary, GUI |

## 3. Architecture (layers)

### Layer 1 — device drivers

**`lib/AD5724R/`** — AD5724R quad 12-bit DAC over SPI.

- Bus: SPI **mode 2** (SCLK idles high, data latched on the falling edge), MSB first,
  ≤ 4 MHz, **24-bit frames** with SYNC low for the whole frame; the write takes effect on
  the SYNC rising edge. LDAC is tied low on the board, so every DAC-register write updates
  the output immediately.
- Frame layout: DB23 R/W, DB21:19 register (000 DAC, 001 range, 010 power, 011 control),
  DB18:16 address (000–011 = A–D, 100 = all), DB15:0 data with the 12-bit code
  **left-justified** (`code << 4`).
- Register values used, from FIRMWARE_HANDOFF §3: output range **`010` = +10.8 V**
  (written to "all" before any code); power control **`0x001F`** (PUREF + PUA–PUD, then
  ≥ 10 µs); control function with clamp enable (DB2) and TSD enable (DB3); control clear
  (REG 011, ADDR 100) as a software clear.
- **Readback**: a frame with DB23 = 1 requests a register; its contents are clocked out on
  SDO during the next frame (a NOP: REG 011, ADDR 000, data 0). Reading power control back
  as `0x001F` in DB4:0 is the proof that the SPI mode and wiring are right. The
  power-control read also carries status: DB5 TSD, DB7–DB10 OCA–OCD (output clamp active).
- "All four" (ADDR 100) is used only for range and clear, never for value writes.

**`lib/ADS1015/`** — ADS1015 4-channel 12-bit ADC over I²C.

- Address **0x48**, registers 0x00 conversion / 0x01 config, 16-bit big-endian.
- Single-shot per reading: config with OS = 1, MUX `1xx` for single-ended AIN0–3, PGA
  **`010` = ±2.048 V** (required by the monitor divider design), MODE = 1, DR = 1600 SPS,
  comparator disabled (ALERT/RDY is unconnected). Poll OS (or wait ~1 ms), read the
  conversion; result is left-justified, `code = (int16_t)raw >> 4`, negative clamped to 0.
- Averages 8–16 conversions per reading for mains rejection (the front end has a ~50 Hz
  corner); all four channels at ~10 Hz fits comfortably.

### Layer 2 — configuration and mapping

**`lib/ValveConfig/`** — one struct, persisted in internal flash as LittleFS file
`/srfm_cal.bin` with a version field and a CRC. Per channel: DAC address (A–D), ADC input
(0–3), `code_full_scale` (default 3851 for CH1–3 / 3831 for CH4), readback gain and offset
(default 3.19 mV/code, 0), `readback_enabled`, pressure endpoints (vMin/vMax/pMin/pMax).
Global: heartbeat timeout (default 2 s). On a missing or invalid record the nominal values
are used and the board reports **`cal=uncal`**. Edited over the host link with `CAL …`,
written with `CAL SAVE`.

**`lib/PressureControl/`** — the linear pressure ↔ valve-voltage map from `main`
(`voltage = vMin + (p − pMin)·(vMax − vMin)/(pMax − pMin)`, clamped to the calibrated
range, vacuum handled as `pMin > pMax`), now keyed by 1-indexed channel and fed from the
ValveConfig endpoints rather than a compile-time table. Valve voltage → DAC code goes
through `code_full_scale` so "10 V" means 10.000 V *at the valve* behind the 100 Ω series
resistor: `code = clamp(round(fraction × code_full_scale), 0, code_full_scale)`.

### Layer 3 — firmware (`src/main.cpp`)

- **Boot** (FIRMWARE_HANDOFF §6, in order): pins to the pulled state → SPI/I²C → watchdog
  → CLR high → power control `0x001F` → range +10.8 V → control (clamp, TSD) → code 0 ×4 →
  power-control readback (fail → `FAULT`, rail stays off) → ADS1015 ACK → SHDN high →
  ~150 ms → FLT + monitor sweep → `!READY ch1=… ch4=…`.
- **States**: `READY` (outputs accepted), `STOPPED` (after `STOP`, needs `START`), `FAULT`
  (eFuse latch, link lost, or boot readback failure; `CLEARFAULT`, or `START` for LINKLOST).
- **Housekeeping loop**: monitor sweep with the per-channel checks (§4 below), DAC
  power-control poll for OC/TSD, FLT poll, heartbeat timer, then the WDT kick.
- **Shutdown** (`STOP`, LINKLOST, FLT): code 0 on all channels **then** SHDN low.
- **Protocol**: line-oriented ASCII, exactly one reply per command (`OK`/`ERR`/`FAULT`),
  `!` events for asynchronous faults. Full table in `PROTOCOL.md`; bench commands
  (`VERIFY`, `SPIMODE`, `RAW`, `RAWGET`, `ADC`, `RAIL`, `DUMP`, `HANG`) exist so
  FIRMWARE_HANDOFF §9 can be walked from a terminal, plus `DFU` (bootloader entry, used by
  the upload hook) and `RAMTEST <SOFT|WDT>` (reset diagnostic). `ID` ends with
  `rst=0x<RESETREAS> bootword=0x<.noinit word>` as seen at boot.
- **Bootloader entry** (`__wrap_enterSerialDfu` / `honourDfuRequest` in `main.cpp`): safe
  state, request file `/dfu_req` in LittleFS (plus a `.noinit` word), stop kicking the WDT →
  watchdog reset → next boot consumes the file before starting the WDT and calls the core's
  real `enterSerialDfu()`. The `.noinit` word is kept as a diagnostic only; it does not
  survive the bootloader pass (§0).

### GUI (`gui/pressure_gui.py`)

tkinter + pyserial, same structure as `main` (reader thread → queue → Tk main loop, four
panels side by side, presets computed from the `REGULATORS` table, JSON profiles), updated
to `PROTOCOL.md`: channels **1–4** everywhere including profiles, `HB` sent every 1 s
while connected, each panel shows the monitor readback (`%` or `n/a`) parsed from `GET`.
The `REGULATORS` table must still be kept in sync with `CAL PRESS` by hand.

## 4. Key design decisions

| Decision | Why |
|---|---|
| **Safe state comes from the board pulls, not firmware** (CLR pull-down, SHDN pull-down, SYNC pull-up) | Any time the MCU pins are high-Z — before boot, in reset, after a WDT reset — the DAC is at 0 V and the 24 V rail is off. Firmware only has to *leave* the safe state deliberately. |
| **Strict boot order**, rail enabled last, and only after the DAC readback proves the SPI link | Valves see 24 V only when their command is already a verified 0 V. A wrong SPI mode or a stuck CLR can never result in an uncontrolled valve. |
| **Stop order: codes to 0, then SHDN low** | An ITV vents on 0 V; cutting 24 V first leaves output pressure "retained temporarily and not guaranteed" (SMC). |
| **SPI mode 2 with register readback as proof** | Matches the AD5724R timing diagram; mode 1 will usually also work. `VERIFY`/`SPIMODE` let the bench settle it without a rebuild. |
| **Channel map is data** (`CAL MAP`, in ValveConfig) | Gerber review found VOUTA/B swapped and AIN0–3 reversed vs. the design docs; the assembled board decides. A board spin or re-ordered valve is a config change. Default `CDBA/3210`, from the SRFMV1 netlist and confirmed on the bench 2026-09-10. |
| **`code_full_scale` per channel, never exceeded** | The +10.8 V range exists to absorb the 100 Ω series drop into the valve's ~6.5 kΩ / ~10 kΩ input, not to over-drive it. Nominal codes are ±~1 %; calibration replaces them. |
| **Heartbeat, default 2 s, `HBT 0` to disable** | USB CDC gives no reliable "host went away". Arms on the first command after boot so a fresh boot with no host does not fault. The GUI feeds it every 1 s. |
| **Watchdog 2 s, kicked only from the main loop** after a successful housekeeping pass | A hang gets the pulls' safe state for free. Never kicked from an ISR. |
| **DFU entry is a watchdog double hop with a flash-backed request**, and the upload hook sends `DFU` instead of the 1200-baud touch | The nRF52 WDT survives a soft reset, the stock bootloader does not feed it, and a WDT reset wipes GPREGRET; `.noinit` RAM measured not to survive the bootloader pass either. A file in LittleFS is the one thing that does. The core's `enterSerialDfu()` is `--wrap`ped onto the same path so Arduino-IDE-style touches still work. |
| **Calibration in LittleFS with version + CRC, `uncal` flag when absent** | Runtime `CAL` on `main` was lost at reset. The host must be able to tell nominal from measured. |
| **Per-channel monitor thresholds** (FIRMWARE_HANDOFF §7): `< 0.5 V` (code < ~157) = open load; `> 5.6 V` (code > ~1750) = wiring fault; `\|measured − expected\| > 10 % F.S. for > 1 s` = stuck; all four `< ~0.3 V` (code < ~100) with SHDN high = no rail | Nominal 0 % is 1.0 V and −6 % is 0.76 V, so 0.5 V is unambiguous. 10 % leaves room for ±6 % monitor + ±1 % valve + settling. Per-channel faults are reported, not acted on; the monitor is never used as feedback. |
| **eFuse latch cleared only by `CLEARFAULT`** | The TPS26600 is in latch-off mode deliberately; automatic retry into a short is not wanted. |
| **Out-of-range commands rejected, not clamped** (`P` excepted, for compatibility with `main`) | A typo in a script must fail loudly, not run the valve at the limit. |
| **Readback individually enable-able**, disabled → `n/a` | CH4's ITV0030-3BL order code appears to lack the monitor option; it must not present as a permanent open-load fault. |

## 5. Toolchain workaround (custom board / variant / linker)

PlatformIO's `nordicnrf52` platform has no XIAO nRF52840 board, and the framework package
it ships (`framework-arduinoadafruitnrf52` 1.6.1) carries only SoftDevice **S140 6.1.1**
headers and a v6 linker script, while the XIAO's stock bootloader flashes **S140 7.3.0**
(application start `0x27000`). The repo therefore carries:

| File | What it does |
|---|---|
| `boards/xiao_nrf52840.json` | `core: nRF5`, `bsp: adafruit`, `variant: Seeed_XIAO_nRF52840` with `variants_dir: variants`, `softdevice: s140 6.1.1, sd_fwid 0x0123`, `ldscript: linker/nrf52840_s140_v7.ld`, `bootloader.settings_addr 0xFF000`, USB VID/PID `0x2886:0x8044` / `0x0044`, `upload: nrfutil`, 1200-bps touch (overridden to off in `platformio.ini`), `maximum_size 811008`, `maximum_ram_size 237568` |
| `variants/Seeed_XIAO_nRF52840/` | Seeed's `variant.h`/`variant.cpp`: `SPI` on D8/D9/D10, `Wire` on D4/D5, `USE_LFXO` |
| `linker/nrf52840_s140_v7.ld` | `FLASH ORIGIN = 0x27000, LENGTH = 0xED000 − 0x27000`; `RAM ORIGIN = 0x20006000`; `.svc_data`/`.fs_data` sections; `INCLUDE "nrf52_common.ld"` from the core |

The 6.1.1 headers are API-compatible for the SoC calls this firmware makes (no BLE; the
SoftDevice is present only because the bootloader expects it). `sd_fwid 0x0123` is what
`adafruit-nrfutil` checks at upload. Upload is serial DFU through the stock bootloader
(`pio run -t upload`, tool fetched by PlatformIO); the fallback is double-tap reset →
`XIAO-SENSE` drive / bootloader port → `pio run -t upload` again, or copy a UF2 built with
`uf2conv.py -f 0xADA52840 -b 0x27000 -c -o firmware.uf2 .pio/build/xiao_nrf52840/firmware.hex`.
SWD is not accessible on the assembled board, so the bootloader is the only recovery path.

**Getting into the bootloader (verified 2026-09-10).** PlatformIO's 1200-baud touch is
disabled (`board_upload.use_1200bps_touch = no`, `wait_for_upload_port = no`). The nRF52
watchdog keeps running across a soft reset and the XIAO's stock bootloader (Adafruit-derived
UF2 bootloader 0.6.1, S140 7.3.0) does not feed it, so touch → soft reset → DFU gets cut off
~2 s in and leaves the bootloader in DFU mode with an invalid app (a second upload then
works, but that is the recovery, not the flow). Instead `tools/upload_dfu.py`
(`extra_scripts`, pre-upload) finds the board by USB VID 0x2886: a bootloader port (PID
0x0044/0x0045) is used as-is; otherwise it opens the application port (PID 0x8044/0x8045),
sends `DFU`, waits up to 15 s for the bootloader port and hands it to `adafruit-nrfutil`.
`--upload-port` is optional. The firmware's `DFU` path is a double hop: outputs and rail to
the safe state, request file `/dfu_req` in internal LittleFS (and a `.noinit` RAM word),
stop kicking the watchdog → watchdog reset (the only reset that stops the WDT) → the freshly
booted firmware sees the file before starting the watchdog, deletes it and performs the
core's normal soft reset into serial DFU. `-Wl,--wrap=enterSerialDfu` routes the core's own
`enterSerialDfu()` through this path, so a 1200-baud touch from other tools (Arduino IDE)
also works, ~3 s slower. Measured: the bootloader clears `NRF_POWER->RESETREAS` before
starting the app (`ID` → `rst=0x0`), and the `.noinit` word reads `0xFFFFFFFF` after a
watchdog reset + bootloader pass (`RAMTEST WDT`), which is why the flash file is the
mechanism that works.

Build outputs: `.pio/build/xiao_nrf52840/firmware.hex` and `firmware.zip` (DFU package).

## 6. Open items

From FIRMWARE_HANDOFF §11 plus what the firmware work surfaced:

| Item | Owner | Impact |
|---|---|---|
| **D7–D10 TVS diodes forward-biased on VOUTA–D** (§0) — every DAC output clamps at ~0.67 V | hardware | **Blocks bring-up step 2 onward.** Rotate 180° if the standoff is ≥ 11 V, fit a 12–15 V unidirectional SMA TVS the right way round, or remove for bring-up. Part number unknown (EasyEDA cloud library) — check before rotating |
| ~~SPI mode to verify~~ — **resolved 2026-09-10**: mode 2, `VERIFY` → `pc=0x001F` | — | Driver default confirmed |
| **Channel map**: DAC side verified 2026-09-10 (`CDBA`, netlist + DMM); ADC side (`3210`) from the netlist, still to confirm with a valve on each pad (FIRMWARE_HANDOFF §9 step 4) | bring-up | Default and saved config are `CDBA/3210`; `CAL MAP` + `CAL SAVE` if step 4 disagrees |
| **CH4 monitor presence** — ITV0030-3BL may lack the monitor output | hardware / purchasing | `CAL RBEN 4` default; until known, expect `n/a` or an open-load report on CH4 |
| ~~WDT vs. bootloader interaction~~ — **resolved 2026-09-10.** The WDT does survive the soft reset and the bootloader does not feed it, so the stock 1200-baud touch was replaced by the `DFU` command + watchdog double hop with a flash-backed request (§5). `pio run -t upload` works from a running WDT-enabled firmware; double-tap UF2 remains the fallback | — | None |
| Update README §9 / SCHEMATIC_SPEC §10 (hardware repo) to the `CDBA/3210` map and the D7–D10 orientation | docs | None — firmware uses the measured map |
| ~~Confirm XIAO plain vs. Sense~~ — **resolved 2026-09-10**: Sense (PID 0x8045 stock); IMU not on D4/D5, no I²C conflict | — | None |
| **Pneumatic shut-off solenoid driver** — not on this board revision | system | Adds one GPIO (D0/D1) to the boot sequence; **commissioning blocker regardless of firmware** — do not connect air until it exists |
| BOM voltage-rating lock at JLC | hardware | None, but the board in hand may carry 25 V-rated caps on the 24 V rail |
| `code_full_scale` and readback gain per channel to calibrate with a DMM | bring-up | Nominal until then; board reports `uncal` if nothing was saved |
| Cross-coupling test result (FIRMWARE_HANDOFF §9 step 6) | hardware | Decides whether the isolated DC-DC footprints get populated |

## 7. Next steps

1. **Fix D7–D10** (§0, §6), then re-run bring-up step 2: `RAW X 2048` + `RAWGET X` + DMM
   on each pad field; the outputs must reach ~5.3 V with no `!FAULT OC`.
2. **Resume bench bring-up** from step 4, no air, MainPower ON — README § Bench bring-up
   walks FIRMWARE_HANDOFF §9 with the exact commands: `RAIL ON` + `ADC n` with a valve on
   each pad → `CAL MAP` / `CAL SAVE` only if the ADC side differs from `3210` → `SET 1 50`
   + `GET 1` → step test with `STATUS` → `HANG`. (Steps 1 and 3 already pass.)
3. **Calibrate** `CAL FS` and `CAL RB` per channel against a DMM, `CAL SAVE`, confirm
   `cal=ok` in `ID`.
4. **GUI against real hardware**: connect, watch `HB` keep the link alive, confirm monitor
   readbacks track presets, run `gui/profiles/quick_check.json`.
5. **Verify the pressure endpoints against a gauge** once air is safe to connect (the
   pneumatic shut-off item in §6 first).
6. Later: pneumatic shut-off GPIO in the boot/stop sequence; GUI calibration editor
   (`CAL GET` → panels) so `REGULATORS` no longer needs manual sync; optional eFuse IMON
   on D1.

Repo: https://github.com/YILINUMICH/SRFM (private).
