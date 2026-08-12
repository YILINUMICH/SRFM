# Test profiles

A profile is a JSON file describing a sequence of held setpoints. The GUI's
**Script…** button loads one and steps through it unattended, so a long test
is written once and replayed instead of being clicked out by hand.

Drop new profiles in this folder — the file dialog opens here by default.

## Minimal example

```json
{
  "name": "hold 100 kPa for five minutes",
  "steps": [
    {"label": "pressurise", "hold_s": 300, "set": [{"reg": 0, "kPa": 100}]}
  ]
}
```

Only `steps` is required, and each step needs only `hold_s`. Everything else
has a default.

## Top-level fields

| Field | Type | Default | Meaning |
|---|---|---|---|
| `steps` | list | *required* | the sequence, run in order; must not be empty |
| `name` | string | the filename | shown in the progress line and the log |
| `loops` | integer ≥ 1 | `1` | how many times to repeat the whole step list |
| `zero_on_finish` | boolean | `true` | send `ZERO` when the run ends or is stopped |

## Step fields

| Field | Type | Default | Meaning |
|---|---|---|---|
| `hold_s` | number > 0 | *required* | how long to hold this step, in seconds |
| `set` | list | `[]` | setpoints applied at the **start** of the step |
| `label` | string | `"step N"` | shown in the progress line and the log |

The setpoints in `set` are applied once, at the moment the step begins; the
rest of `hold_s` is just waiting. An empty `set` holds whatever the previous
step left in place, which is how you write a dwell:

```json
{"label": "settle", "hold_s": 60, "set": []}
```

## Setpoint entries

Each entry in `set` is one of two forms.

**Pressure** — goes through the regulator calibration, same as the `P`
command and the GUI's preset buttons:

```json
{"reg": 0, "kPa": 100}
```

| `reg` | Regulator | Valid `kPa` |
|---|---|---|
| 0 | air, SMC ITV0030-3BL | `1` … `500` |
| 1 | vacuum 1, SMC ITV2090-312L5 | `-1.3` … `-80` |
| 2 | vacuum 2, SMC ITV2090-312L5 | `-1.3` … `-80` |
| 3 | vacuum 3, SMC ITV2090-312L5 | `-1.3` … `-80` |

**Raw voltage** — bypasses the pressure mapping and drives a DAC channel
directly, same as the `V` command. Useful for the spare channels 4–15 or for
debugging:

```json
{"ch": 4, "volts": 2.5}
```

`ch` is `0`–`15` and `volts` is `0` … `10`. Channels 0–3 are the four
regulators, so writing a raw voltage there fights the pressure layer — use
`reg`/`kPa` for those unless you are deliberately debugging.

A single step can mix both forms and address as many channels as you like;
they are all applied together at the start of the step:

```json
{
  "label": "air up, all three vacuums down",
  "hold_s": 600,
  "set": [
    {"reg": 0, "kPa": 150},
    {"reg": 1, "kPa": -40},
    {"reg": 2, "kPa": -40},
    {"reg": 3, "kPa": -40},
    {"ch": 5, "volts": 1.0}
  ]
}
```

## Validation

The whole file is checked **before** the run starts — including every
setpoint against that regulator's calibrated range — so a typo fails in the
file dialog rather than eight hours into a soak. Messages name the step:

```
step 3: 40 kPa is outside regulator 1's range of -80 .. -1.3 kPa
step 1: 'hold_s' must be greater than zero seconds
step 2: a 'set' entry needs either 'reg' or 'ch'
'loops' must be a whole number of 1 or more
```

The vacuum sign is the easy one to get wrong: vacuum setpoints are
**negative**. Writing `40` where you meant `-40` is rejected, not clamped.

## While it runs

The manual controls lock so nothing fights the script. The progress line
shows the loop, the step and the time left:

```
Vacuum soak, 3 channels  —  loop 2/4,  step 3/5 'draw vacuum on all three',  7:41 left
```

**Stop** ends the run within a quarter second. The outputs are zeroed on
finish, on Stop, on disconnect and on closing the window (unless you set
`zero_on_finish` to `false`).

Holds are timed against monotonic deadlines rather than by counting ticks,
so a long soak will not drift.

## Included profiles

| File | Duration | What it does |
|---|---|---|
| `quick_check.json` | ~55 s | Bring-up check: steps the air regulator, one vacuum, and a spare channel, exercising both the pressure and raw-voltage paths. Run this first after any wiring change. |
| `vacuum_soak.json` | ~1 h 45 m | 4 loops of pressurise → vacuum → deep vacuum → vent across all three vacuum channels. |
