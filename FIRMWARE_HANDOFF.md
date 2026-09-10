# Firmware Handoff — 4-Channel Pneumatic Valve Controller

Companion to `README.md` (decisions) and `SCHEMATIC_SPEC.md` (values). This document is
what firmware needs from the hardware: pins, bus settings, register sequences, transfer
functions, safety invariants, and the things the board *cannot* do for you.

Datasheets: AD5724R Rev. G, ADS1015 (TI SBAS473), TPS26600 (SLVSDG2G), SMC ITV2090 /
ITV0030 manuals. Register bit positions below are from those datasheets; the "verify"
tags mark the handful of items to re-check on the bench before trusting.

---

## 0. Read this first — three things that override the design docs

| # | Issue | What firmware does about it |
|---|---|---|
| 1 | **Pin map.** README §9 and SCHEMATIC_SPEC §10 disagree. README §9 is the as-built table (matches refdes R4/R5/R10–R17 and review-log #10). **SCHEMATIC_SPEC §10 is stale — ignore it.** | Use the table in §1 below |
| 2 | **Channel map must be measured, not assumed.** Gerber review (2026-08-16) shows DAC VOUTA/VOUTB swapped and ADS1015 AIN0–AIN3 fully reversed relative to README §9 / SCHEMATIC_SPEC §10, which both say "in order". | Bring-up steps 2 and 4 (§9) establish the map on the assembled board; §5 gives the expected result and the data structure to hold it |
| 3 | **CH4 (ITV0030-3BL) may have no monitor output** — the order code appears to lack the monitor digit. | Readback per channel must be individually enable-able; a disabled channel is reported as `n/a`, never as an open-load fault |

---

## 1. MCU and pin map (as built — README §9)

**MCU:** Seeed XIAO nRF52840 (confirm plain vs. Sense; on Sense, verify the IMU is on the
internal bus, not D4/D5). Soldered down; **SWD is not accessible.** Recovery is the
module's reset button → UF2 bootloader. Do not ship firmware that disables USB.

| Pad | Port | Signal | Direction | Board pull / series | Notes |
|---|---|---|---|---|---|
| D0 | P0.02 / AIN0 | spare | — | — | analog-capable |
| D1 | P0.03 / AIN1 | spare | — | — | reserved for eFuse IMON (DNP network) |
| D2 | P0.28 | DAC **SYNC** (CS, active low) | out | 10 kΩ pull-**up** to 3V3 | Idle high. Drive high on boot before touching SPI |
| D3 | P0.29 | DAC **CLR** (active low) | out | 10 kΩ pull-**down** to GND | **Low = all outputs cleared to 0 V and all SPI writes ignored.** Firmware drives it high as an explicit boot step |
| D4 | P0.04 | I²C **SDA** | bidir | 4.7 kΩ to 3V3 (on-board) | BSP default `Wire` pin. Do not enable internal pull-ups |
| D5 | P0.05 | I²C **SCL** | out | 4.7 kΩ to 3V3 (on-board) | BSP default `Wire` pin |
| D6 | P1.11 | eFuse **SHDN** | out | R4 1 kΩ series; 10 kΩ pull-down; MainPower switch to GND | **High = 24 V rail enabled.** Switch OFF grounds the pin regardless of GPIO (GPIO then sources ~3.3 mA — harmless) |
| D7 | P1.12 | eFuse **FLT** (active low, open-drain) | in | R5 100 Ω series; 100 kΩ pull-up to 3V3 | Low = fault latched (OC, UVLO, OVP, reverse, thermal). No internal pull needed |
| D8 | P1.13 | SPI **SCK** | out | 33 Ω series | BSP default `SPI` |
| D9 | P1.14 | SPI **MISO** ← DAC SDO | in | 33 Ω series | BSP default `SPI` |
| D10 | P1.15 | SPI **MOSI** → DAC SDIN | out | 33 Ω series | BSP default `SPI` |

Not connected to the MCU: DAC LDAC (tied low → each DAC register write updates the output
on the SYNC rising edge), ADS1015 ALERT/RDY (poll instead), `VINgood` / `FuseGood` LEDs
(no GPIO — firmware infers rail state from readback, §6).

**Consequence of the pulls:** any time the MCU's GPIOs are high-Z — before boot, during a
reset, after a watchdog reset — CLR is asserted (DAC at 0 V) and SHDN is low (24 V rail
off). This is the hardware safe state. Firmware only has to *leave* it deliberately and in
the right order.

---

## 2. Framework recommendation

Any of these work; the board has no requirement beyond USB CDC, one SPI, one I²C, GPIO,
a watchdog and a little non-volatile storage.

| Option | Pros | Cons |
|---|---|---|
| **Arduino, Seeed nRF52 (non-mbed / Adafruit-Bluefruit-derived) core** | TinyUSB CDC, `SPI`/`Wire` on the right default pads, `InternalFileSystem` (LittleFS) for calibration, UF2 flashing, fastest to a working rig | Less control of the WDT and USB descriptors; fine for a lab controller |
| Zephyr (`xiao_ble`) | Proper WDT, shell, NVS, USB CDC-ACM samples | Longer setup; overkill unless it's already your toolchain |

Assumed below: Arduino-style API names, but everything is expressed at the register level
so it ports.

---

## 3. AD5724R — SPI, register map, transfer function

### Bus settings

| Parameter | Value |
|---|---|
| Clock | 1–4 MHz is plenty (part allows 30 MHz; the 33 Ω series R and a soldered-down TSSOP don't need speed) |
| Bit order | MSB first |
| Frame | **24 bits**, SYNC low for the whole frame, SYNC high after the 24th clock. The write takes effect on the SYNC rising edge |
| Mode | SCLK idles high, data is latched on the **falling** SCLK edge → **SPI Mode 2** (CPOL=1, CPHA=0). Mode 1 also latches on falling edges and will usually work, but Mode 2 matches the datasheet timing diagram. **Verify with a register readback (below) on first bring-up** |
| Between frames | Keep SYNC high ≥ ~30 ns — any GPIO toggle satisfies this |

### 24-bit input word

```
DB23     R/W        0 = write, 1 = read
DB22     0
DB21:19  REG        000 DAC register   001 output range   010 power control   011 control
DB18:16  ADDR       000 DAC A  001 DAC B  010 DAC C  011 DAC D  100 all four
DB15:0   DATA       AD5724R is 12-bit: code is LEFT-justified in DB15:DB4, DB3:0 = 0
```

### Registers you will use

| Register | REG | ADDR | DATA | Notes |
|---|---|---|---|---|
| Output range | 001 | per channel or 100 (all) | DB2:0 = **010 → +10.8 V** | 000 +5 V, 001 +10 V, 010 +10.8 V, 011 ±5 V, 100 ±10 V, 101 ±10.8 V. Must be written **before** any DAC value |
| Power control | 010 | 000 | DB0 PUA, DB1 PUB, DB2 PUC, DB3 PUD, **DB4 PUREF** | All default to **0 = powered down**, including the reference. A write to a powered-down channel is silently ignored. Write **0x001F**, then wait ≥ 10 µs |
| Power control (read) | 010 | 000 | DB5 TSD, DB7 OCA, DB8 OCB, DB9 OCC, DB10 OCD | Read-only status: thermal shutdown and per-channel overcurrent (clamp). Poll this in the housekeeping loop |
| Control — function | 011 | 001 | DB0 SDO disable, DB1 CLR select, DB2 **clamp enable**, DB3 **TSD enable** | Recommend DB2 = 1 (20 mA output clamp on a shorted line) and DB3 = 1. Leave DB1 = 0 (clear → 0 V in unipolar ranges). Verify bit positions against Table "Control register functions", Rev. G |
| Control — clear | 011 | 100 | — | Software clear: all outputs to 0 V (same effect as the CLR pin) |
| Control — load | 011 | 101 | — | Not needed; LDAC is tied low |
| DAC value | 000 | channel or 100 | code << 4 | Output updates on SYNC rising edge |

**Readback:** send a frame with DB23 = 1 and the target REG/ADDR; the requested contents
are clocked out on SDO during the *next* frame (send a NOP: REG 011, ADDR 000, DATA 0).
Use this to prove the SPI mode and the power-control write on bring-up.

### Transfer function (per channel)

```
+10.8 V range, 12-bit:   V_dac  = code × 10.8 / 4096         = 2.637 mV / LSB
Series 100 Ω into valve:  V_valve = V_dac × Z_in / (Z_in + 100)

CH1–CH3  ITV2090  Z_in ≈ 6.5 kΩ  →  10.000 V at valve needs 10.152 V  →  nominal code 3851
CH4      ITV0030  Z_in ≈ 10 kΩ   →  10.000 V at valve needs 10.101 V  →  nominal code 3831
```

Store a per-channel `code_full_scale` (default 3851 / 3831) and compute

```
code = clamp( round( target_fraction × code_full_scale ), 0, code_full_scale )
```

**Never command above `code_full_scale`.** The +10.8 V range exists to absorb the series-R
loss, not to over-drive the valve. SMC gives Z_in as "approx." with no tolerance, so the
default codes are ±~1 % — the per-channel calibration (§8) replaces them.

Valve command semantics: 0 V → valve vents to zero output pressure. So code 0 is the safe
command as well as the zero-pressure command.

---

## 4. ADS1015 — I²C, configuration, transfer function

| Parameter | Value |
|---|---|
| Address | **0x48** (ADDR → GND) |
| I²C clock | 100 or 400 kHz, either is fine |
| Registers | 0x00 conversion (read), 0x01 config (read/write), 16-bit big-endian |
| Pointer | first byte after address selects the register |

### Config register — single-shot, one channel

```
bit 15   OS      write 1 = start conversion;  read 1 = idle/complete, 0 = converting
bit 14:12 MUX    100 AIN0–GND  101 AIN1–GND  110 AIN2–GND  111 AIN3–GND  (single-ended)
bit 11:9  PGA    010 = FSR ±2.048 V   ← required by the divider design (SPEC §8)
bit 8     MODE   1 = single-shot
bit 7:5   DR     100 = 1600 SPS (default; conversion ≈ 0.6 ms)
bit 4:0   comparator: 0 0 0 11 (disabled — ALERT/RDY is unconnected)
```

Per reading: write config with OS=1 and the MUX for the channel, poll config until OS=1
(or just wait 1 ms), read the conversion register. Result is 12 bits **left-justified** in
the 16-bit word: `code = (int16_t)raw >> 4`. Single-ended inputs use 0…2047 only; a
negative code means the input is slightly below ground (noise) — clamp to 0.

### Transfer function (nominal, before calibration)

```
V_pin      = code × 1.000 mV
V_monitor  = code × 3.19 mV                    (÷0.3139 divider incl. ADC input Z)
%F.S.      = (V_monitor − 1.0 V) / 4.0 V × 100

code 314  ≈ 1.00 V  ≈   0 % F.S.
code 1569 ≈ 5.00 V  ≈ 100 % F.S.
code 1645 ≈ 5.24 V  ≈ 100 % + 6 % (worst-case in-spec maximum)
```

The monitor is **±6 % F.S.** — a liveness/sanity signal, not feedback. Do not close a loop
on it. Averaging: the front end has a ~50 Hz corner; average 8–16 conversions per reading
for mains rejection. Anything beyond that is wasted against ±6 % source accuracy.

Sample rate target: all four channels at ~10 Hz. At 1600 SPS with 16-sample averaging
that is ~40 ms of conversion per sweep — comfortably fits.

---

## 5. Channel map — expected from Gerber review; **confirm on the bench (§9 steps 2 and 4)**

Logical channels are **1-indexed CH1–CH4** in every host-facing message. The Gerber
connectivity review (2026-08-16) found the board is **not** wired sequentially — layout
chose this to avoid trace crossings. Treat the table below as the starting values for the
bench test, not as ground truth: the map is decided by what the assembled board measures.

| Logical | Valve | DAC output / U3 pin | AD5724R ADDR field (DB18:16) | ADC input / U4 pin | ADS1015 MUX bits (14:12) |
|---|---|---|---|---|---|
| CH1 | ITV2090 | VOUT **B** / 4 | 001 | **AIN3** / 7 | 111 |
| CH2 | ITV2090 | VOUT **A** / 3 | 000 | **AIN2** / 6 | 110 |
| CH3 | ITV2090 | VOUT C / 23 | 010 | **AIN1** / 5 | 101 |
| CH4 | ITV0030 | VOUT D / 22 | 011 | **AIN0** / 4 | 100 |

"ADDR" is the address field inside every 24-bit SPI word sent to the DAC, not a separate
register. "MUX" is the input-select field of the ADS1015 config register (pointer 0x01).

```c
// AD5724R ADDR field per logical channel (A=0, B=1, C=2, D=3), index 0 = CH1
static const uint8_t dac_addr[4] = { 1, 0, 2, 3 };
// ADS1015 single-ended MUX code per logical channel (AIN0=4 ... AIN3=7), index 0 = CH1
static const uint8_t adc_mux[4]  = { 7, 6, 5, 4 };
```

Keep these in the calibration/config struct (§8) and make them settable over the host
link, so the bench result is entered as data — no rebuild. README §9 and SCHEMATIC_SPEC §10 both still say "in order" — that text is wrong.

Do not use the "all four" DAC ADDR (100) for value writes; it is fine for range and clear.

## 6. Boot sequence — the part that matters for safety

The hardware guarantees DVCC before AVDD and holds the safe state until firmware acts.
Firmware must leave the safe state in exactly this order:

| # | Action | Why |
|---|---|---|
| 1 | Configure GPIOs: SYNC=high, CLR=**low**, SHDN=**low** as outputs; FLT input | Explicitly reproduce what the pulls were already doing, so nothing glitches when the pin becomes an output |
| 2 | Init SPI (Mode 2, MSB first, ≤4 MHz) and I²C | |
| 3 | Start the watchdog (§7) | Before anything can go wrong |
| 4 | Drive **CLR high** | Until this, every SPI write is ignored — first thing to suspect if the DAC "does nothing" |
| 5 | Write **power control = 0x001F** (PUREF + PUA–PUD). Wait ≥ 10 µs | Channels and the reference default to power-down |
| 6 | Write **output range = +10.8 V**, address "all" | Must precede any code |
| 7 | Write **control function**: clamp enable, TSD enable | Optional but recommended |
| 8 | Write **code 0** to all four DAC registers | Belt-and-braces: outputs are already 0 V from CLR, but this makes the registers match |
| 9 | Read back power control → confirm 0x001F in DB4:0 | Proves SPI mode and wiring. If it fails, **stop here** and do not enable the rail |
| 10 | Configure ADS1015 (write a config with OS=0 to check the ACK) | |
| 11 | Drive **SHDN high**. The 24 V rail ramps over ~90 ms | Valves see supply only after the command is already 0 V. No-op if MainPower is OFF |
| 12 | Wait ~150 ms, read FLT, sweep all four monitors, report `READY` with per-channel status | Confirms each valve is alive before the host can command anything |

**Shutdown / e-stop (`STOP` command, host disconnect, or any fault):** write code 0 to all
channels (or assert CLR), **then** drive SHDN low. Order: command to zero *before* removing
the rail — the valve vents on 0 V, and removing 24 V first leaves the output pressure
"retained temporarily and not guaranteed" per SMC.

**Remember what this does NOT do:** electrical off is not pneumatic off. An unpowered ITV
passes supply pressure through. The pneumatic shut-off solenoid (README §8) is not on this
board revision; until it exists, do not commission with air connected. When it is added,
it is intended to sit on a spare GPIO (D0/D1) driving a low-side FET with a gate
pull-down — so a firmware hang or reset closes the air.

---

## 7. Runtime invariants and fault handling

### Watchdog
Enable the nRF52840 WDT (1–2 s). A WDT reset returns every GPIO to high-Z, which asserts
CLR (DAC → 0 V) and drops SHDN (24 V off). That is the correct response to a hung
firmware and costs nothing — the pulls do the work. Kick the WDT only from the main loop
after a successful housekeeping pass, not from a timer ISR.

### Host link loss
USB CDC gives no reliable "host went away" signal. Implement a **command heartbeat**: if no
valid command arrives within a configurable timeout (default 2 s, disable-able for bench
use), execute the shutdown sequence and report `FAULT LINKLOST`. Loss of USB power itself
is handled by hardware (3V3 collapses → both rails off).

### eFuse FLT
FLT low means the TPS26600 has **latched off** (MODE = latch-off, deliberately). The rail
does not come back by itself. Recovery: firmware sets all DAC codes to 0, drives SHDN low
for ≥ 100 ms, then high again — **only on an explicit host `CLEARFAULT` command**, never
automatically. Report which of these it might be; firmware can't distinguish them:
overcurrent/short (≥0.99 A), UVLO (<19.9 V), OVP (>27.9 V), reverse polarity, thermal.

Also FLT-low-with-no-fault: the MainPower switch OFF grounds SHDN, and a disabled eFuse
does not assert FLT — but the rail is dead. Distinguish by readback (below).

### Rail-alive inference (no GPIO for `FuseGood`)
If all four monitors read below ~0.3 V (code < ~100) with SHDN high, the 24 V rail is off
(switch OFF, latched fault, or barrel unplugged). One channel low with the others normal
is an open load on that channel.

### Per-channel monitor checks (each sweep)

| Condition | Threshold (nominal) | Meaning |
|---|---|---|
| V_monitor < 0.5 V | code < ~157 | Open load / dead valve / no rail — nominal 0 % is 1.0 V, −6 % is 0.76 V, so 0.5 V is unambiguous |
| V_monitor > 5.6 V | code > ~1750 | Out of spec high — wiring fault (24 V on the monitor line clamps at the ADC, will read near full scale) |
| \|measured − expected\| > 10 % F.S. for > 1 s after a step | — | Stuck valve or lost supply pressure. Expected = commanded fraction; 10 % leaves room for ±6 % monitor + ±1 % valve + pneumatic settling |

Readback on a disabled channel (CH4 if the ITV0030 lacks the monitor option) is skipped
and reported as `n/a`.

### DAC status
Poll the power-control register every housekeeping pass: OCx set = that output is being
clamped (shorted command line); TSD set = thermal shutdown. Report as a channel fault;
do not auto-retry.

### Command limits
Reject and report, never silently clamp, any host command outside 0–100 % (or outside a
per-channel configurable max).

---

## 8. Calibration and non-volatile data

Two per-channel gain terms are known-deterministic and must be calibrated once with the
valve attached:

| Term | Source of error | Nominal | Procedure |
|---|---|---|---|
| `code_full_scale[ch]` | 100 Ω series R against "approx." valve Z_in | 3851 (6.5 kΩ) / 3831 (10 kΩ) | Command a code, measure V at the valve's pin 2 with a DMM, adjust so 10.000 V at the valve. Linear, so one point at full scale is enough |
| `readback_gain[ch]` | ADC input impedance (−1.17 %) + 1 % divider resistors | 3.19 mV/code | Measure V at valve pin 4 with a DMM at ~50 % and ~100 % command; fit gain (and optionally offset) to the ADC code |

Store in flash (LittleFS / NVS) as a small struct with a version field and a CRC; on
missing/invalid data, fall back to the nominal values above **and report `UNCAL`** in
status so the host knows. Include the channel map (§5) and `readback_enabled[ch]` in the
same struct so a board spin or a re-ordered CH4 valve is a config change.

Expose `CAL SET`, `CAL GET`, `CAL SAVE`, `CAL DEFAULT` over the host protocol.

---

## 9. Bring-up and channel-map verification (no air connected)

Do these in order, with the MainPower switch OFF until step 4.

1. **SPI proof:** boot to step 9 of §6. Readback of power control must return PUREF+PUA–PUD.
   If not: check CLR is actually high, then try Mode 1, then scope SYNC/SCK/MOSI.
2. **DAC channel map:** with SHDN still low (AVDD is up regardless — it follows 3V3), write
   code 2048 to DAC address A only. DMM each valve pad field's pin 2 (white). Whichever
   pad reads ~5.3 V is the physical channel wired to VOUTA — expect CH2. Repeat for B, C,
   D and record the result in the config struct. §5 says what to expect; what you measure
   wins. Disagreement with §5 is not a firmware problem — note it and carry on with the
   measured map.
3. **I²C proof:** ADS1015 ACKs at 0x48; a conversion with MUX=AIN0 and nothing connected
   reads near 0.
4. **Rail and readback map:** switch ON, drive SHDN high, confirm `FuseGood` LED. Connect
   one valve at a time to CH1's pads; the ADS1015 channel that rises to ~1 V (0 % F.S.) is
   the one wired to CH1. Repeat for each pad field.
5. **Loopback sanity:** command 50 % on a channel; its monitor should read ~3 V ± 6 %.
6. **Cross-coupling test** (README §7): step one channel 0 → 100 % while logging all four
   monitors. The other three should not move. This is the instrumented test that decides
   whether the isolated DC-DC footprints get populated.
7. **Watchdog test:** stall the main loop on a debug command; confirm the rail drops and
   the DAC clears within the WDT period, and the board re-enumerates.

---

## 10. Host protocol (proposal — adjust to taste)

Line-oriented ASCII over USB CDC, `\n` terminated, one response line per command,
responses start with `OK`, `ERR <reason>`, or `FAULT <reason>`. Keep it human-typeable so
a serial terminal is a valid host.

| Command | Response | Notes |
|---|---|---|
| `SET <ch> <pct>` | `OK` | 1–4, 0.0–100.0. Rejected if channel faulted or board not READY |
| `SETALL <p1> <p2> <p3> <p4>` | `OK` | Written sequentially (LDAC is tied low, so not strictly simultaneous; ~10 µs apart, far below valve response) |
| `GET <ch>` | `OK <cmd_pct> <mon_pct or n/a> <status>` | |
| `STATUS` | one line per channel + rail/FLT/uncal flags | |
| `STOP` | `OK` | Zero all, SHDN low. Requires `START` to resume |
| `START` | `OK` / `FAULT …` | Re-runs §6 steps 8–12 |
| `CLEARFAULT` | `OK` / `FAULT …` | Only path that toggles SHDN after a latch |
| `HB` | `OK` | Heartbeat; any valid command also counts |
| `CAL …` | | §8 |
| `ID` | firmware version, channel map, cal state | |

Unsolicited lines (prefixed `!`) for asynchronous faults: `!FAULT FLT`, `!FAULT OPENLOAD 3`,
`!FAULT LINKLOST`.

---

## 11. Open items firmware depends on

| Item | Owner | Firmware impact |
|---|---|---|
| Update README §9 / SCHEMATIC_SPEC §10 to the §5 map | docs | None — firmware uses §5 |
| Confirm ITV0030 (CH4) monitor option present or absent | hardware / purchasing | `readback_enabled[4]` default |
| Confirm XIAO plain vs. Sense | hardware | I²C pad conflict check |
| Pneumatic shut-off solenoid driver (not on this rev) | system | Adds one GPIO output to the boot sequence; **commissioning blocker** regardless of firmware |
| BOM voltage-rating lock at JLC | hardware | None, but note the board in hand may carry 25 V-rated caps on the 24 V rail |
