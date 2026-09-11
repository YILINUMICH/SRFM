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

- [`FIRMWARE_HANDOFF.md`](docs/FIRMWARE_HANDOFF.md) — hardware → firmware handoff:
  pin map, register sequences, boot order, safety invariants, bring-up.
- [`PROTOCOL.md`](docs/PROTOCOL.md) — the host command set.

## Project layout

| Path | What it is |
|---|---|
| `platformio.ini` | PlatformIO config, env `xiao_nrf52840` (nordicnrf52, Adafruit nRF52 core) |
| `firmware/boards/xiao_nrf52840.json` | Board definition — PlatformIO has no stock XIAO nRF52840 |
| `firmware/variants/Seeed_XIAO_nRF52840/` | Pin-map variant, copied from Seeed's Arduino core |
| `firmware/linker/nrf52840_s140_v7.ld` | Linker script placing the application at `0x27000` (SoftDevice S140 7.x) |
| `firmware/tools/upload_dfu.py` | PlatformIO pre-upload hook: finds the board by USB VID, sends `DFU`, waits for the bootloader port (see [Deployment](#deployment)) |
| `hardware/SRFM_PCB/` | Board design: V1.1 Gerbers (2026-09-11, D7–D10 orientation fixed), the netlist `Netlist_Schematic1_2026-09-11.tel` and the EasyEDA project. The V1 files the assembled board was built from are in git history (`hardware/SRFMV1/` at commit e6ce3fd) |
| `firmware/lib/AD5724R/` | AD5724R SPI DAC driver (24-bit frames, SPI mode 2, register readback) |
| `firmware/lib/ADS1015/` | ADS1015 I²C ADC driver (single-shot, averaging) |
| `firmware/lib/ValveConfig/` | Persisted calibration/config struct (channel map, full-scale codes, readback gain/enable, pressure endpoints, heartbeat timeout) — version + CRC in LittleFS `/srfm_cal.bin` |
| `firmware/lib/PressureControl/` | Pressure ↔ valve-voltage linear mapping |
| `firmware/src/main.cpp` | Firmware: boot sequence, watchdog, heartbeat, fault handling, host protocol |
| `hardware/datasheets/` | SMC ITV0030 / ITV2090 catalogue pages (set-pressure ranges, sensitivity, monitor spec) |
| `gui/pressure_gui.py` | tkinter GUI (needs `pip install -r gui/requirements.txt`) |
| `gui/profiles/` | JSON test profiles for long unattended runs, with their own [README](gui/profiles/README.md) |
| `data/` | CSV recordings written by the GUI, one per connection (created on first use, git-ignored) |
| `docs/FIRMWARE_HANDOFF.md` | Hardware → firmware handoff (authoritative for pins, registers, safety) |
| `docs/PROTOCOL.md` | Host protocol (authoritative for commands and replies) |
| `docs/HANDOFF.md` | Project handoff: architecture, design decisions, open items, bench status |

Top level: `platformio.ini` stays at the root so `pio run` works from a fresh
checkout; it points PlatformIO at `firmware/` (`src_dir`, `lib_dir`,
`boards_dir`). Everything else is grouped by what it is — `firmware/`,
`gui/`, `hardware/`, `docs/` — with `data/` for recordings.

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
[`FIRMWARE_HANDOFF.md`](docs/FIRMWARE_HANDOFF.md) §1–§4.

### Channels

Channels are **1-indexed CH1–CH4** in every host message, matching the
silkscreen.

| CH | Valve | Firmware name | 0–10 V at the valve maps to | DAC output (series R) | ADC input |
|---|---|---|---|---|---|
| 1 | SMC ITV2090-312L5 (vacuum) | `VAC1` | −1.3 … −80 kPa | VOUT **C** (R18) | AIN **3** |
| 2 | SMC ITV2090-312L5 (vacuum) | `VAC2` | −1.3 … −80 kPa | VOUT **D** (R19) | AIN **2** |
| 3 | SMC ITV2090-312L5 (vacuum) | `VAC3` | −1.3 … −80 kPa | VOUT **B** (R20) | AIN **1** |
| 4 | SMC ITV0030-3BL (air pressure) | `AIR` | +1 … +500 kPa | VOUT **A** (R21) | AIN **0** |

> **The physical map above is verified.** It comes from the board netlist
> (`hardware/SRFM_PCB/Netlist_Schematic1_2026-09-11.tel`, unchanged from V1: VOUTC→R18→CMD1, VOUTD→R19→CMD2, VOUTB→R20→CMD3,
> VOUTA→R21→CMD4; AIN3…AIN0 ← RD1…RD4) and was confirmed on the bench on
> 2026-09-10 (VOUTB measured on the CH3 pads). It is the firmware default
> (`map=CDBA/3210`). The Gerber-review prediction in FIRMWARE_HANDOFF §5
> (`BACD`) was wrong on the DAC side.

The SMC catalogue (`hardware/datasheets/`) lists the 1–5 V analog monitor
output as standard on the ITV0000 series, so CH4's ITV0030-3BL should have
one; the order code has no monitor option digit either way. If the bench
shows nothing on its RD4 pad, its readback can be disabled (`CAL RBEN 4 0`),
in which case it reports `n/a` and never raises an open-load fault.

## Deployment

From a fresh checkout to a running system.

**1. Install the tools.** Needs Python 3.9 or newer:

```sh
pip install -r gui/requirements.txt     # pyserial, for the GUI
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
pre-upload hook `firmware/tools/upload_dfu.py` finds the board by USB VID 0x2886: if a
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
on the bench 2026-09-10; see [`HANDOFF.md`](docs/HANDOFF.md) § Bench status.

The toolchain is not stock: the repo carries the board JSON, the pin variant
and a linker script because PlatformIO does not know the XIAO nRF52840 and
its core package only ships SoftDevice 6.1.1 headers, while the XIAO's
bootloader carries S140 7.3.0. The 6.1.1 headers are API-compatible for the
SoC calls this firmware makes; the linker script puts the application at
`0x27000` where the 7.x SoftDevice expects it. See [`HANDOFF.md`](docs/HANDOFF.md)
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

## A typical session

What happens from Connect to Disconnect, in order. Everything here is
covered in more detail under [GUI](#gui) and in
[`PROTOCOL.md`](docs/PROTOCOL.md).

1. **Connect.** The GUI sends `ID` and `GET`, then `STREAM 100`. The board
   starts sending one timestamped sample per 10 ms and the panels, the plot
   and the recording all run off that. The first sample opens
   `data/srfm_<date>_<time>.csv`; the bottom bar shows the file and its row
   count. Nothing to press. (If the board runs firmware without `STREAM`,
   the GUI says so once and polls `GET` at 2 Hz instead.)
2. **Set pressures.** Preset buttons, a typed pressure (clamped to the
   valve's range and rounded to its step — the panel says if it changed
   what you typed), or a raw command voltage for debugging. Each panel shows
   three readback lines: *commanded* (the setpoint), *valve* (the 0–10 V
   command at the valve's input pin) and *monitor* (what the valve reports
   back on its 1–5 V monitor pin, as volts, % of span and kPa).
3. **Watch.** *Live plot…* shows the last 60 s of command vs. readback per
   channel. `!FAULT` lines land in the log and in the bar at the top; a
   stuck valve, open monitor line or lost rail is reported, not acted on.
4. **Run a profile** for anything long: *Script…* loads a JSON sequence of
   held setpoints from `gui/profiles/`, locks the manual controls while it
   runs, and *Stop* ends it within a quarter second. Step markers go into
   the recording as event rows next to the samples.
5. **Stop.** `CLEAR` zeroes one valve, `ZERO ALL` zeroes all four with the
   rail up, `STOP` zeroes then drops the 24 V rail (`START` brings it
   back), `CLEARFAULT` is the only way out of a latched eFuse fault.
6. **Disconnect** (or close the window, which also sends `ZERO`). The GUI
   sends `STREAM 0`, the CSV closes, and the next Connect starts a new one.

**The data.** One CSV per connection. Sample rows carry host time, seconds
since the file opened, the board's millisecond stamp (`fw_ms`, the clock to
use for rate work) and per channel `set_kPa, cmd_V, mon_V, mon_pct,
mon_kPa`; every logged line (commands sent, replies, faults, profile
markers) is an event row with the channel columns empty. In pandas,
`df[df.event == ""]` is the numeric data and `df[df.event != ""]` the
timeline. 100 Hz streaming is roughly 60 MB per hour.

**From a terminal instead** (`pio device monitor`, or any serial tool):
send `HBT 0` first so the 2 s link-loss timer does not stop the board
between hand-typed commands, then `STREAM 100` to watch the `~` lines,
`STREAM 0` to stop them. Only one program can hold the port, so close the
terminal before starting the GUI.

**Rates, for reference.** The board sweeps the four monitors at 10 Hz on
its own and at 100 Hz while streaming (ADS1015 at 3300 SPS, 2 samples
averaged). The GUI records every sample, redraws the panels at 10 Hz and
the plot at 4 Hz. Faster than 100 Hz gains nothing: the monitor front end
rolls off around 50 Hz, the valves respond in ~0.1 s, and the monitor is a
±6 % F.S. signal.

## Safety

The rules the firmware implements (FIRMWARE_HANDOFF §6–§7). Read them before
changing `firmware/src/main.cpp`.

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

> **Where it stands (2026-09-11):** steps 1–4 and 7 pass on the assembled
> board. The TVS diodes D7–D10 (fitted backwards, clamping every DAC output
> at ~0.67 V) have been reworked and all four outputs sweep 0–10.15 V clean;
> the channel map `CDBA/3210` is measured on both sides and saved in flash.
> Remaining: step 5 (loopback with real valves), step 6 (cross-coupling),
> then `CAL FS` / `CAL RB` per channel. The `STREAM` firmware added
> 2026-09-11 compiles but has not been flashed or run on the board yet.
> Details in [`HANDOFF.md`](docs/HANDOFF.md) § Bench status.

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
you measured. *Bench 2026-09-10: PASS. VOUTB seen on the CH3 pads,
confirming the netlist. Before the D7–D10 rework the pads only tracked the
DAC below ~0.67 V with `!FAULT OC n` above; after it every output sweeps
0–10.15 V with the clamp bit never set, with a +0.6 % gain error that
`CAL FS` absorbs.*

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
AIN3 (netlist: AIN3/2/1/0 ← RD1/2/3/4; *bench 2026-09-10: PASS, confirmed
by jumpering a driven CMD pad to each RD pad in turn*). Repeat
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
`!READY`. Reconnect the monitor afterwards. *Bench 2026-09-10: PASS — the USB
port dropped at 1.15 s and the board was back, READY with the rail up, at
1.98 s.*

## Host protocol (115200, one reply line per command)

Summary only — replies, edge cases and the full bench command set are in
[`PROTOCOL.md`](docs/PROTOCOL.md). Commands are case-insensitive; every command
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
| `STREAM <hz>` | 0–200, 0 = off: one `~<ms> <GET body>` line per period on the board's clock (`PROTOCOL.md` § Stream lines). 100 Hz is the intended rate |
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
**monitor readback** alongside the commanded value: the monitor-pin voltage,
the percent of full scale, and the pressure derived from that voltage
(1 V = 0 %, 5 V = 100 % over the panel's calibrated range), e.g.
`3.118 V  (53.0 %)  ≈ -43.0 kPa`. Below 0.5 V it says `no monitor signal`
(no valve, rail off, or open line); `n/a` means readback is disabled for
that channel. The three readback lines are: **commanded** — the pressure
setpoint the firmware is holding (from `P`, or back-derived from the code
after `V`/`C`); **valve** — the 0–10 V command voltage at the valve's input
pin; **monitor** — what the valve reports back on its 1–5 V monitor pin
(its own pressure sensor, ±6 % F.S.), shown as volts, % of span, and the
pressure that voltage means on this channel's calibrated range.

A typed pressure is corrected before it is sent: clamped to the valve's
set-pressure range (ITV2090 −1.3 … −80 kPa, ITV0030 1 … 500 kPa) and
rounded to the valve's step (0.1 kPa vacuum, 1 kPa air — SMC rate both at
0.2 % F.S. sensitivity, so finer input is not a different pressure at the
valve; the DAC itself resolves ~0.02 / ~0.13 kPa). The entry is rewritten
with the value actually sent and the panel says what changed and why. The
step lives in the `REGULATORS` table next to the endpoints.

**Stream 100 Hz** (checkbox in the connection bar, on by default) asks the
firmware for `STREAM 100`: the board then sends one timestamped sample line
per 10 ms on its own clock, and the GUI sends `HB` once a second to keep
the link alive. Every sample goes to the recording and the plot history;
the panels are redrawn at 10 Hz and the plot at 4 Hz so the display does
not eat the CPU. If no sample has arrived for 2 s the heartbeat tick
re-sends `STREAM 100` (a fresh connection, or the board has been through
`START`); an `ERR` to that means older firmware and the GUI says so once
and falls back to polling. Sample lines are never logged.

**Live readback** (checkbox, on by default) is the polling fallback used
when streaming is off or unsupported: `GET` twice a second, panels
refreshed silently — those polls and their replies are kept out of the log,
a `GET` you press yourself is still logged. The poll doubles as the
heartbeat; with both boxes off the GUI sends `HB` every second instead.
Either way the link-loss timer stays armed at its default, so unplugging
the cable or killing the GUI zeroes the outputs within ~2 s.

**Live plot…** opens a 2×2 window, one strip chart per channel, showing the
last 60 s of the command voltage (blue) and the readback (red) on the same
0–10 V scale. The readback is the valve's monitor pin mapped onto the
command range (1 V → 0 V, 5 V → 10 V); the raw monitor voltage is in each
pane's header, and the red trace is left blank while there is no monitor
signal. It is fed by whatever is delivering samples — the 100 Hz stream, or
the 2 Hz poll when streaming is off — and redraws four times a second.
Plain Tk canvases, no extra dependency.

**Recording is automatic.** The first status reply after Connect opens
`data/srfm_<YYYYmmdd>_<HHMMSS>.csv` at the repository root (git-ignored)
and Disconnect, a lost port, or closing the window closes it; the bottom
bar shows the file name and row count while it is open. Every sample
becomes a row — 100 Hz while streaming, 2 Hz when polling — with the
board's millisecond stamp in `fw_ms` on streamed rows (the clock to use
for rate work; blank on polled and event rows) and, per channel, the
commanded pressure, the command voltage, the
monitor volts, the monitor % of span and the pressure derived from the
monitor (blank without a signal). Everything that reaches the log
(commands sent, acks, `ERR`/`FAULT` replies, `!` events, profile step
markers) is written as its own row with the text in the `event` column,
so a fault sits next to the readings around it. Columns:
`time, t_s, fw_ms, VAC1_set_kPa, VAC1_cmd_V, VAC1_mon_V, VAC1_mon_pct,
VAC1_mon_kPa, … AIR_…, event`. Rows are flushed as written, so a crash
or a pulled cable keeps everything up to the last reply.

`CLEAR` drops one valve back to zero, `ZERO ALL` is
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
| Flashing | avrdude | serial DFU via bootloader (`DFU` command + watchdog double hop, `firmware/tools/upload_dfu.py`), UF2 fallback; no SWD |
| Board state | always live | `READY` / `STOPPED` / `FAULT` gate every output command |
