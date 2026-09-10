# HANDOFF — SRFM Valve Controller, custom PCB branch

**Date:** 2026-09-10
**Branch:** `custom-pcb-nrf52840` (the Mega 2560 + LTC2668 EVM version stays on `main`)
**Status:** Firmware compiles clean. **Not yet tested on hardware — bench bring-up pending**
(README § Bench bring-up / FIRMWARE_HANDOFF §9).

Companion documents, both authoritative over this one where they overlap:

- `FIRMWARE_HANDOFF.md` — what the hardware needs from firmware: pin map, register
  sequences, boot order, fault handling, calibration, bring-up. Its §0 lists three things
  that override the older design docs (stale pin table, unmeasured channel map, CH4 monitor
  possibly absent).
- `PROTOCOL.md` — the host command set, replies and events.

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
| `platformio.ini` | env `xiao_nrf52840`: `nordicnrf52@~10.9.0`, Arduino framework (Adafruit nRF52 core, `framework-arduinoadafruitnrf52` 1.6.1), `upload_protocol = nrfutil`, monitor 115200 |
| `boards/xiao_nrf52840.json` | board definition (see §5) |
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
  (`VERIFY`, `SPIMODE`, `RAW`, `ADC`, `RAIL`, `DUMP`, `HANG`) exist so FIRMWARE_HANDOFF §9
  can be walked from a terminal.

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
| **Channel map is data** (`CAL MAP`, in ValveConfig) | Gerber review found VOUTA/B swapped and AIN0–3 reversed vs. the design docs; the assembled board decides. A board spin or re-ordered valve is a config change. Default `BACD/3210`. |
| **`code_full_scale` per channel, never exceeded** | The +10.8 V range exists to absorb the 100 Ω series drop into the valve's ~6.5 kΩ / ~10 kΩ input, not to over-drive it. Nominal codes are ±~1 %; calibration replaces them. |
| **Heartbeat, default 2 s, `HBT 0` to disable** | USB CDC gives no reliable "host went away". Arms on the first command after boot so a fresh boot with no host does not fault. The GUI feeds it every 1 s. |
| **Watchdog 2 s, kicked only from the main loop** after a successful housekeeping pass | A hang gets the pulls' safe state for free. Never kicked from an ISR. |
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
| `boards/xiao_nrf52840.json` | `core: nRF5`, `bsp: adafruit`, `variant: Seeed_XIAO_nRF52840` with `variants_dir: variants`, `softdevice: s140 6.1.1, sd_fwid 0x0123`, `ldscript: linker/nrf52840_s140_v7.ld`, `bootloader.settings_addr 0xFF000`, USB VID/PID `0x2886:0x8044` / `0x0044`, `upload: nrfutil`, 1200-bps touch, `maximum_size 811008`, `maximum_ram_size 237568` |
| `variants/Seeed_XIAO_nRF52840/` | Seeed's `variant.h`/`variant.cpp`: `SPI` on D8/D9/D10, `Wire` on D4/D5, `USE_LFXO` |
| `linker/nrf52840_s140_v7.ld` | `FLASH ORIGIN = 0x27000, LENGTH = 0xED000 − 0x27000`; `RAM ORIGIN = 0x20006000`; `.svc_data`/`.fs_data` sections; `INCLUDE "nrf52_common.ld"` from the core |

The 6.1.1 headers are API-compatible for the SoC calls this firmware makes (no BLE; the
SoftDevice is present only because the bootloader expects it). `sd_fwid 0x0123` is what
`adafruit-nrfutil` checks at upload. Upload is serial DFU through the stock bootloader
(`pio run -t upload`, tool fetched by PlatformIO); the fallback is double-tap reset →
`XIAO-SENSE` drive → copy a UF2 built with
`uf2conv.py -f 0xADA52840 -b 0x27000 -c -o firmware.uf2 .pio/build/xiao_nrf52840/firmware.hex`.
SWD is not accessible on the assembled board, so the bootloader is the only recovery path.

Build outputs: `.pio/build/xiao_nrf52840/firmware.hex` and `firmware.zip` (DFU package).

## 6. Open items

From FIRMWARE_HANDOFF §11 plus what the firmware work surfaced:

| Item | Owner | Impact |
|---|---|---|
| **SPI mode to verify** on the bench (`VERIFY`; `SPIMODE 1` if mode 2 fails) | bring-up | Driver default; boot refuses to enable the rail until the readback passes |
| **Channel map to measure** (FIRMWARE_HANDOFF §9 steps 2 and 4), then `CAL MAP` + `CAL SAVE` | bring-up | Default `BACD/3210` is the Gerber-review prediction, not a measurement |
| **CH4 monitor presence** — ITV0030-3BL may lack the monitor output | hardware / purchasing | `CAL RBEN 4` default; until known, expect `n/a` or an open-load report on CH4 |
| **WDT vs. bootloader interaction.** The nRF52 WDT keeps running through a soft reset, so the 1200-baud touch into the bootloader relies on the bootloader feeding the WDT during DFU | bring-up | If `pio run -t upload` fails after a WDT-enabled firmware is on the board, use the double-tap UF2 route |
| Update README §9 / SCHEMATIC_SPEC §10 (hardware repo) to the §5 map | docs | None — firmware uses the measured map |
| Confirm XIAO plain vs. Sense (on Sense, check the IMU is not on D4/D5) | hardware | I²C pad conflict |
| **Pneumatic shut-off solenoid driver** — not on this board revision | system | Adds one GPIO (D0/D1) to the boot sequence; **commissioning blocker regardless of firmware** — do not connect air until it exists |
| BOM voltage-rating lock at JLC | hardware | None, but the board in hand may carry 25 V-rated caps on the 24 V rail |
| `code_full_scale` and readback gain per channel to calibrate with a DMM | bring-up | Nominal until then; board reports `uncal` if nothing was saved |
| Cross-coupling test result (FIRMWARE_HANDOFF §9 step 6) | hardware | Decides whether the isolated DC-DC footprints get populated |

## 7. Next steps

1. **Bench bring-up**, no air, MainPower OFF until step 4 — README § Bench bring-up walks
   FIRMWARE_HANDOFF §9 with the exact commands: `VERIFY` → `RAW` + DMM → `ADC 0` →
   `RAIL ON` + `ADC n` → `CAL MAP` / `CAL SAVE` → `SET 1 50` + `GET 1` → step test with
   `STATUS` → `HANG`.
2. **Calibrate** `CAL FS` and `CAL RB` per channel against a DMM, `CAL SAVE`, confirm
   `cal=ok` in `ID`.
3. **GUI against real hardware**: connect, watch `HB` keep the link alive, confirm monitor
   readbacks track presets, run `gui/profiles/quick_check.json`.
4. **Verify the pressure endpoints against a gauge** once air is safe to connect (item 7
   in §6 first).
5. Later: pneumatic shut-off GPIO in the boot/stop sequence; GUI calibration editor
   (`CAL GET` → panels) so `REGULATORS` no longer needs manual sync; optional eFuse IMON
   on D1.

Repo: https://github.com/YILINUMICH/SRFM (private).
