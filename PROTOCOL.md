# SRFM host protocol — custom PCB (XIAO nRF52840 + AD5724R + ADS1015)

Line-oriented ASCII over USB CDC, 115200 (baud is ignored by CDC), `\n` or `\r`
terminated. Commands are case-insensitive. **Every command gets exactly one reply
line** starting with `OK`, `ERR <reason>` or `FAULT <reason>`, so a host can run in
send→read-line lockstep. Asynchronous events are extra lines prefixed `!`.

Channels are **1-indexed, CH1–CH4**, matching the board silkscreen and
`FIRMWARE_HANDOFF.md` §5:

| CH | Valve | Firmware name | 0–10 V at the valve maps to |
|---|---|---|---|
| 1 | SMC ITV2090-312L5 (vacuum) | `VAC1` | −1.3 … −80 kPa |
| 2 | SMC ITV2090-312L5 (vacuum) | `VAC2` | −1.3 … −80 kPa |
| 3 | SMC ITV2090-312L5 (vacuum) | `VAC3` | −1.3 … −80 kPa |
| 4 | SMC ITV0030-3BL (air pressure) | `AIR` | +1 … +500 kPa |

Which physical DAC output / ADC input serves each CH is data (`CAL MAP`), not code.

## Board state

| State | Meaning | `SET`/`P`/`V` accepted? |
|---|---|---|
| `READY` | DAC proven, 24 V rail enabled | yes |
| `STOPPED` | after `STOP`: all outputs 0, rail off | no — `START` first |
| `FAULT` | eFuse FLT latched, host link lost, or DAC readback failed at boot | no — `CLEARFAULT` (or `START` for LINKLOST) |

Boot ends with an unsolicited `!READY ...` or `!FAULT ...` line (§ Events).

## Operating commands

| Command | Reply | Notes |
|---|---|---|
| `ID` | `OK SRFM-PCB v2.0 state=<state> map=<d1><d2><d3><d4>/<a1><a2><a3><a4> cal=<ok\|uncal> hb=<s\|off>` | `map` = DAC output letter (A–D) and ADC input number (0–3) per CH1..CH4, e.g. `BACD/3210` |
| `SET <ch> <pct>` | `OK ch<n> pct=<p> code=<c>` | 0.0–100.0 % of the valve's full scale. Out of range → `ERR`, never clamped |
| `SETALL <p1> <p2> <p3> <p4>` | `OK` | four percentages |
| `P <ch> <pressure>` | `OK <name> p=<applied> v=<valve volts>` | engineering units (kPa) through the pressure calibration; **clamped** to the calibrated range like the old firmware, reply shows what was applied |
| `V <ch> <volts>` | `OK ch<n> v=<volts> code=<c>` | voltage **at the valve**, 0–10 V, out of range → `ERR` |
| `C <ch> <code>` | `OK ch<n> code=<c> v=<volts>` | raw 12-bit DAC code, 0..`code_full_scale[ch]`, out of range → `ERR` |
| `GET` | `OK VAC1=<p>,<v>V,<mon>; VAC2=…; VAC3=…; AIR=…` | per channel: commanded pressure, commanded valve voltage, monitor as `<pct>%` of F.S. or `n/a` |
| `GET <ch>` | `OK ch<n> cmd=<pct> mon=<pct\|n/a> status=<status>` | status: `ok`, `open`, `high`, `stuck`, `oc`, `n/a`, `norail` |
| `STATUS` | `OK state=<state> rail=<on\|off> flt=<0\|1> dac=0x<pc> cal=<ok\|uncal> hb=<s\|off> ch1=<cmd>,<mon>,<status> … ch4=…` | one line; `dac` is the raw power-control readback (0x001F = healthy) |
| `ZERO` | `OK all zero` | every channel to 0 %, rail stays on (GUI panic button) |
| `STOP` | `OK stopped` | all channels 0 **then** rail off (handoff §6 order). Needs `START` to resume |
| `START` | `OK ready` or `FAULT <reason>` | re-runs boot steps 8–12 and reports READY |
| `CLEARFAULT` | `OK ready` or `FAULT <reason>` | only path that cycles SHDN after an eFuse latch: codes 0, SHDN low ≥100 ms, high, re-check |
| `HB` | `OK` | heartbeat. Any valid command also counts |
| `HBT <seconds>` | `OK hb=<s\|off>` | link-loss timeout, 0 = off. Timer arms on the first command after boot. Persist with `CAL SAVE` |

## Calibration (`CAL …`, persisted in internal flash)

| Command | Reply | Notes |
|---|---|---|
| `CAL GET` | `OK cal=<ok\|uncal> ch1=fs:<code>,rb:<mV>/<off>,rben:<0\|1>,dac:<A-D>,adc:<0-3>,p:<vMin>/<vMax>/<pMin>/<pMax> … ch4=…` | |
| `CAL FS <ch> <code>` | `OK` | `code_full_scale`: DAC code that puts 10.000 V at the valve (default 3851 CH1–3, 3831 CH4). ≤ 4095 |
| `CAL RB <ch> <mV_per_code> <offset_code>` | `OK` | monitor gain (default 3.19) and offset (default 0): `V_mon = (code − off) × mV/1000` |
| `CAL RBEN <ch> <0\|1>` | `OK` | monitor readback enabled; 0 reports `n/a` (CH4 ITV0030 may have no monitor) |
| `CAL MAP <ch> <A-D> <0-3>` | `OK` | physical DAC output and ADS1015 input for a logical channel. Rejected if it would duplicate another channel's DAC output |
| `CAL PRESS <ch> <vMin> <vMax> <pMin> <pMax>` | `OK` | pressure↔valve-voltage endpoints. Legacy form `CAL <ch> <vMin> <vMax> <pMin> <pMax>` also accepted |
| `CAL SAVE` | `OK saved` | write to flash (`/srfm_cal.bin`, versioned + CRC) |
| `CAL DEFAULT` | `OK defaults` | nominal values in RAM (not saved until `CAL SAVE`) |

`cal=uncal` means flash held no valid record and the nominal values are in use.

## Bench / bring-up commands (FIRMWARE_HANDOFF.md §9)

| Command | Reply | Notes |
|---|---|---|
| `VERIFY` | `OK verify=<yes\|no> pc=0x<readback>` | re-reads the DAC power-control register; `yes` when DB4:0 = 0x1F |
| `SPIMODE <1\|2>` | `OK spimode=<m> verify=<yes\|no>` | re-init SPI in that mode, re-run the DAC bring-up and the readback proof (step 1). Persist the working mode with `CAL SAVE` |
| `RAW <A-D> <code>` | `OK dac<X> code=<c> vdac=<volts>` | write a code to a **physical** DAC output, bypassing the channel map (step 2). 0–4095; use with no air connected |
| `ADC <0-3>` | `OK ain<n> code=<c> v=<pin volts>` | one raw single-ended conversion on a physical ADS1015 input (steps 3–4) |
| `RAIL <ON\|OFF>` | `OK rail=<on\|off>` | drive SHDN directly (step 4). `ON` refused while FLT is latched |
| `DUMP` | `OK ch1=<code>,<dacX>,<ainN>,<adc code>,<mon V> ; …` | raw per-channel view |
| `HANG` | `OK hanging` | stalls the main loop to prove the watchdog (step 7): rail drops, DAC clears, board re-enumerates within ~2 s |

## Events (unsolicited, prefixed `!`)

| Line | When |
|---|---|
| `!READY ch1=<status> ch2=… ch3=… ch4=…` | end of boot / `START` / `CLEARFAULT` |
| `!FAULT FLT` | eFuse fault latched (OC ≥0.99 A, UVLO <19.9 V, OVP >27.9 V, reverse, thermal — firmware cannot tell which) |
| `!FAULT LINKLOST` | heartbeat timeout: outputs zeroed, rail off |
| `!FAULT OPENLOAD <ch>` | monitor < 0.5 V on one channel while the others are alive |
| `!FAULT NORAIL` | all monitors < ~0.3 V with SHDN high: switch off, latched fuse or barrel unplugged |
| `!FAULT STUCK <ch>` | monitor differs from the command by >10 % F.S. for >1 s |
| `!FAULT OC <ch>` / `!FAULT TSD` | DAC output clamp active (shorted command line) / DAC thermal shutdown |

Per-channel faults are reported, not acted on. FLT and LINKLOST run the shutdown
sequence (codes 0, then rail off).
