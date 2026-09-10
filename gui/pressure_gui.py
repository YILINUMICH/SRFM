#!/usr/bin/env python3
"""SRFM regulator control GUI.

Talks to the custom SRFM PCB (Seeed XIAO nRF52840 + AD5724R 4-channel DAC +
ADS1015 monitor ADC) over USB CDC serial. Line-based protocol, one reply per
command — see PROTOCOL.md at the repository root.

Requires: pyserial  (pip install -r requirements.txt)
"""

import json
import os
import queue
import re
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import serial
import serial.tools.list_ports

BAUD = 115200  # USB CDC ignores it, but pyserial wants a number

COPYRIGHT = "© Soft Robot Face Mask User Interface, Aug 2026, Y. Ma  UofM"

# Command-signal range at the valve. The firmware scales this onto the DAC
# code through CAL FS; both SMC regulator models take a 0-10 V input.
VOLT_MIN, VOLT_MAX = 0.0, 10.0

# The firmware drops the 24 V rail if it hears nothing for the heartbeat
# timeout (HBT, default 2 s). Sending every second leaves a safe margin.
HEARTBEAT_MS = 1000

# Delay after opening the port before the first command. The XIAO does not
# reset when the port opens, so this only has to cover CDC settling.
CONNECT_SETTLE_MS = 500

# Preset buttons wrap onto a second line after this many. The four regulator
# panels are laid out in a single row, so a panel has to stay narrow enough
# that four of them fit across an ordinary screen.
PRESETS_PER_ROW = 3

# One entry per channel CH1..CH4, in channel order (index = ch - 1):
# (firmware name, display name, pressure at 0V, pressure at 10V, unit, presets)
#
# The firmware name is what GET puts before '=' and is used to match a status
# reply to its panel. The two pressures are in calibration order — pressure at
# vMin then at vMax, matching the firmware's CAL PRESS endpoints. They are NOT
# sorted numerically: the vacuum units run from -1.3 kPa at 0V down to
# -80 kPa at 10V, and showing them in that order is what makes the range
# readable next to the valve voltage.
REGULATORS = [
    ("VAC1", "Vacuum 1 (ITV2090)", -1.3, -80.0, "kPa", (-1.3, -10, -20, -40, -60, -80)),
    ("VAC2", "Vacuum 2 (ITV2090)", -1.3, -80.0, "kPa", (-1.3, -10, -20, -40, -60, -80)),
    ("VAC3", "Vacuum 3 (ITV2090)", -1.3, -80.0, "kPa", (-1.3, -10, -20, -40, -60, -80)),
    ("AIR", "Air pressure (ITV0030)", 1.0, 500.0, "kPa", (1, 50, 100, 200, 350, 500)),
]
NUM_CHANNELS = len(REGULATORS)
STATUS_NAMES = [reg[0] for reg in REGULATORS]


def to_float(text):
    """Parse a float, or None if the text is not a number."""
    try:
        return float(text.strip())
    except ValueError:
        return None


def is_partial_number(text):
    """Keystroke filter: allow anything that could still become a number.

    Rejects letters and stray symbols as they are typed, but permits the
    intermediate states ("", "-", ".", "-.") you pass through while entering
    a value like -12.5.
    """
    return text in ("", "-", ".", "-.", "+") or to_float(text) is not None


def fmt_hms(seconds):
    """Seconds as m:ss, or h:mm:ss once past an hour."""
    seconds = int(max(0, seconds))
    hours, rest = divmod(seconds, 3600)
    minutes, secs = divmod(rest, 60)
    return f"{hours}:{minutes:02d}:{secs:02d}" if hours else f"{minutes}:{secs:02d}"


# --- reply parsing ---------------------------------------------------------
#
# Pure functions, kept out of the App class so they can be exercised without
# a display. The protocol is one reply line per command, starting with OK,
# ERR or FAULT, plus unsolicited '!' event lines (PROTOCOL.md).

def parse_get_reply(body):
    """Split a GET reply body into {firmware name: (pressure, volts, monitor)}.

    "VAC1=-40.00,5.000V,49.1%; VAC2=-1.30,0.000V,1.6%; ...; AIR=250.50,5.000V,n/a"
    -> {"VAC1": ("-40.00", "5.000", "49.1%"), ...}

    Channels are matched by the name before '=', not by position. Parts that
    do not carry a known name are skipped, so any other reply that happens to
    contain '=' and ',' (a STATUS line, say) yields an empty dict.
    """
    found = {}
    for part in body.split(";"):
        name, sep, rhs = part.strip().partition("=")
        name = name.strip().upper()
        if not sep or name not in STATUS_NAMES:
            continue
        fields = [field.strip() for field in rhs.split(",")]
        if len(fields) < 2:
            continue
        volts = fields[1].rstrip("Vv")
        monitor = fields[2] if len(fields) > 2 and fields[2] else "n/a"
        found[name] = (fields[0], volts, monitor)
    return found


def monitor_text(monitor, p_at_0v, p_at_10v, unit):
    """Monitor field of a GET reply as display text.

    The firmware reports the valve's analog monitor as a percentage of full
    scale — the same 0-100 % the 0-10 V command spans — so the equivalent
    pressure comes from the panel's own endpoints:
    "49.1%" -> "49.1 % ≈ -40.0 kPa". Anything else ("n/a") is shown as-is.
    """
    text = monitor.strip()
    if not text:
        return "n/a"
    if not text.endswith("%"):
        return text
    pct = to_float(text[:-1])
    if pct is None:
        return text
    pressure = p_at_0v + (p_at_10v - p_at_0v) * pct / 100.0
    return f"{pct:.1f} % ≈ {pressure:.1f} {unit}"


def classify_reply(line):
    """Sort a line from the controller into what the GUI should do with it.

    Returns (kind, payload):
      "event"   an unsolicited '!' line (payload: the line)
      "ok"      a bare "OK" — the heartbeat ack, or any bodyless success
      "status"  a GET reply (payload: parse_get_reply()'s dict)
      "ack"     a single-command ack that changed an output; follow with GET
      "log"     anything else — ERR, FAULT, ID, VERIFY, STATUS, DUMP ...
    """
    if line.startswith("!"):
        return "event", line
    if line == "OK":
        return "ok", None
    if not line.startswith("OK "):
        return "log", None
    body = line[3:].strip()

    # DUMP: "ch1=<code>,<dacX>,<ainN>,<adc code>,<mon V> ; ..." — raw view,
    # log only. Checked before the generic "ch" ack test below.
    if body.startswith("ch1=") and "," in body:
        return "log", None

    # P -> "VAC1 p=-40.00 v=5.000"; V/C/SET -> "ch1 v=5.000 code=1926";
    # ZERO -> "all zero"; STOP -> "stopped"; START/CLEARFAULT -> "ready".
    # Each changes the outputs, so pull a full status afterwards to keep
    # every panel in step, including the ones the command did not touch.
    if (" p=" in body and " v=" in body) or body.startswith("ch") \
            or body in ("all zero", "stopped", "ready"):
        return "ack", None

    status = parse_get_reply(body)
    if status:
        return "status", status
    return "log", None


_STATE_RE = re.compile(r"\bstate=([A-Za-z]+)")


def reply_state(line):
    """Board state named in an ID or STATUS reply ("state=READY"), or None."""
    match = _STATE_RE.search(line)
    return match.group(1).upper() if match else None


# --- test profiles -------------------------------------------------------
#
# A profile is a JSON file describing a sequence of held setpoints, so a long
# unattended run can be defined once and replayed. Format:
#
#   {
#     "name": "vacuum soak",
#     "loops": 2,                     optional, default 1
#     "zero_on_finish": true,         optional, default true
#     "steps": [
#       {"label": "pressurise", "hold_s": 30,
#        "set": [{"reg": 4, "kPa": 100}]},
#       {"label": "soak",       "hold_s": 600,
#        "set": [{"reg": 4, "kPa": 100}, {"reg": 1, "kPa": -40}]},
#       {"label": "vent",       "hold_s": 15, "set": []}
#     ]
#   }
#
# A "set" entry is either {"reg": 1-4, "kPa": <pressure>} for the pressure
# layer (P command), or {"ch": 1-4, "volts": <volts>} to drive a channel's
# valve voltage directly (V command). Channels are 1-indexed like the board
# silkscreen: 1-3 are the vacuum units, 4 is air. An empty "set" holds
# whatever the previous step left in place.

PROFILE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "profiles")


def _require_number(value, where, field):
    """Return value as a float, or raise naming the offending field.

    bool is excluded deliberately: it subclasses int, and a JSON true is not
    a setpoint.
    """
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{where}: '{field}' must be a number")
    return float(value)


def _require_channel(value, where, field):
    """Return value as a channel number 1..NUM_CHANNELS, or raise."""
    if not isinstance(value, int) or isinstance(value, bool) \
            or not 1 <= value <= NUM_CHANNELS:
        raise ValueError(f"{where}: '{field}' must be 1..{NUM_CHANNELS}")
    return value


def _validate_step(step, i):
    where = f"step {i + 1}"
    if not isinstance(step, dict):
        raise ValueError(f"{where}: must be a JSON object")

    hold = _require_number(step.get("hold_s"), where, "hold_s")
    if hold <= 0:
        raise ValueError(f"{where}: 'hold_s' must be greater than zero seconds")

    raw_actions = step.get("set", [])
    if not isinstance(raw_actions, list):
        raise ValueError(f"{where}: 'set' must be a list")

    actions = []
    for entry in raw_actions:
        if not isinstance(entry, dict):
            raise ValueError(f"{where}: every 'set' entry must be a JSON object")

        if "reg" in entry:
            reg = _require_channel(entry["reg"], where, "reg")
            value = _require_number(entry.get("kPa"), where, "kPa")
            _, _, p_at_0v, p_at_10v, unit, _ = REGULATORS[reg - 1]
            lo, hi = min(p_at_0v, p_at_10v), max(p_at_0v, p_at_10v)
            if not lo <= value <= hi:
                raise ValueError(
                    f"{where}: {value:g} {unit} is outside regulator {reg}'s "
                    f"range of {lo:g} .. {hi:g} {unit}")
            actions.append(("P", reg, value))

        elif "ch" in entry:
            channel = _require_channel(entry["ch"], where, "ch")
            value = _require_number(entry.get("volts"), where, "volts")
            if not VOLT_MIN <= value <= VOLT_MAX:
                raise ValueError(
                    f"{where}: {value:g} V is outside {VOLT_MIN:g} .. {VOLT_MAX:g} V")
            actions.append(("V", channel, value))

        else:
            raise ValueError(f"{where}: a 'set' entry needs either 'reg' or 'ch'")

    return {
        "label": str(step.get("label") or f"step {i + 1}"),
        "hold_s": float(hold),
        "actions": actions,
    }


def load_profile(path):
    """Read and validate a test profile.

    Everything is checked up front — setpoints included — so a typo is caught
    before the run starts rather than eight hours in.
    """
    with open(path, "r", encoding="utf-8") as handle:
        try:
            raw = json.load(handle)
        except json.JSONDecodeError as exc:
            raise ValueError(f"not valid JSON: {exc}") from exc

    if not isinstance(raw, dict):
        raise ValueError("the top level must be a JSON object")

    raw_steps = raw.get("steps")
    if not isinstance(raw_steps, list) or not raw_steps:
        raise ValueError("'steps' must be a non-empty list")

    loops = raw.get("loops", 1)
    if not isinstance(loops, int) or isinstance(loops, bool) or loops < 1:
        raise ValueError("'loops' must be a whole number of 1 or more")

    steps = [_validate_step(step, i) for i, step in enumerate(raw_steps)]

    return {
        "name": str(raw.get("name") or os.path.basename(path)),
        "loops": loops,
        "steps": steps,
        "zero_on_finish": bool(raw.get("zero_on_finish", True)),
        "duration_s": sum(step["hold_s"] for step in steps) * loops,
    }


class SerialLink:
    """Background reader thread around a pyserial port."""

    def __init__(self, on_line):
        self.ser = None
        self.on_line = on_line
        self._thread = None
        self._closing = False

    @property
    def connected(self):
        return self.ser is not None and self.ser.is_open

    def connect(self, port):
        self.disconnect()
        self._closing = False
        self.ser = serial.Serial(port, BAUD, timeout=0.2)
        self._thread = threading.Thread(target=self._read_loop, daemon=True)
        self._thread.start()

    def disconnect(self):
        self._closing = True
        if self.ser is not None:
            try:
                self.ser.close()
            except serial.SerialException:
                pass
            self.ser = None

    def send(self, line):
        ser = self.ser
        if ser is None or not ser.is_open:
            raise RuntimeError("not connected")
        ser.write((line + "\n").encode("ascii"))

    def _read_loop(self):
        ser = self.ser
        while ser is not None and ser.is_open and not self._closing:
            try:
                raw = ser.readline()
            except (serial.SerialException, TypeError, OSError, AttributeError):
                # close() runs on the main thread while this read is blocked.
                # pyserial's win32 backend tears down _overlapped_read as it
                # closes, so the in-flight read surfaces as an AttributeError
                # from inside pyserial rather than a clean SerialException.
                break
            if raw:
                self.on_line(raw.decode("ascii", errors="replace").strip())


class RegulatorPanel(ttk.LabelFrame):
    """One regulator: presets, manual pressure, manual voltage, readback."""

    def __init__(self, master, channel, name, p_at_0v, p_at_10v, unit,
                 presets, on_set_pressure, on_set_voltage):
        super().__init__(master, text=f" CH{channel}  {name} ")
        self.channel = channel
        self.unit = unit
        self.p_at_0v = p_at_0v
        self.p_at_10v = p_at_10v
        self.p_lo = min(p_at_0v, p_at_10v)
        self.p_hi = max(p_at_0v, p_at_10v)
        self.on_set_pressure = on_set_pressure
        self.on_set_voltage = on_set_voltage

        vcmd = (self.register(is_partial_number), "%P")

        # The panels sit side by side, so everything here is stacked to keep a
        # panel about a quarter of the window wide.
        ttk.Label(
            self,
            text=(f"Range    {p_at_0v:g} … {p_at_10v:g} {unit}\n"
                  f"Valve    {VOLT_MIN:g} … {VOLT_MAX:g} V"),
            justify="left",
        ).grid(row=0, column=0, columnspan=5, padx=10, pady=(6, 4), sticky="w")

        # --- preset buttons: one click straight to a working setpoint ---
        ttk.Label(self, text="Presets").grid(
            row=1, column=0, padx=(10, 6), pady=2, sticky="nw")
        preset_bar = ttk.Frame(self)
        preset_bar.grid(row=1, column=1, columnspan=4, padx=(0, 10), pady=2, sticky="w")
        self.controls = []
        for n, value in enumerate(presets):
            button = ttk.Button(
                preset_bar,
                text=f"{value:g} {unit}\n{self.voltage_for(value):.2f} V",
                width=9,
                command=lambda v=value: self.apply_pressure(v))
            button.grid(row=n // PRESETS_PER_ROW, column=n % PRESETS_PER_ROW,
                        padx=2, pady=1)
            self.controls.append(button)

        # --- manual pressure entry ---
        ttk.Label(self, text="Pressure").grid(
            row=2, column=0, padx=(10, 6), pady=2, sticky="w")
        self.p_entry = ttk.Entry(self, width=10, justify="right",
                                 validate="key", validatecommand=vcmd)
        self.p_entry.insert(0, "0.0")
        self.p_entry.grid(row=2, column=1, pady=2, sticky="w")
        self.p_entry.bind("<Return>", lambda _e: self.apply_pressure_entry())
        ttk.Label(self, text=unit, width=4).grid(row=2, column=2, padx=(6, 0), sticky="w")
        p_button = ttk.Button(self, text="Set", width=7,
                              command=self.apply_pressure_entry)
        p_button.grid(row=2, column=3, padx=(4, 2), sticky="w")

        # --- clear this valve on its own, without disturbing the other three ---
        clear_button = ttk.Button(self, text="CLEAR", width=7, command=self.clear)
        clear_button.grid(row=2, column=4, padx=(2, 10), sticky="w")
        self.controls += [self.p_entry, p_button, clear_button]

        # --- manual voltage entry (bypasses the pressure mapping) ---
        ttk.Label(self, text="Voltage").grid(
            row=3, column=0, padx=(10, 6), pady=2, sticky="w")
        self.v_entry = ttk.Entry(self, width=10, justify="right",
                                 validate="key", validatecommand=vcmd)
        self.v_entry.insert(0, "0.000")
        self.v_entry.grid(row=3, column=1, pady=2, sticky="w")
        self.v_entry.bind("<Return>", lambda _e: self.apply_voltage_entry())
        ttk.Label(self, text="V", width=4).grid(row=3, column=2, padx=(6, 0), sticky="w")
        v_button = ttk.Button(self, text="Set V", width=7,
                              command=self.apply_voltage_entry)
        v_button.grid(row=3, column=3, padx=(4, 2), sticky="w")
        self.controls += [self.v_entry, v_button]

        self.readback = ttk.Label(self, foreground="#444", justify="left")
        self.readback.grid(row=4, column=0, columnspan=5, padx=10, pady=(6, 2),
                           sticky="w")
        self.show_readback("—", "—", "—")
        self.msg = ttk.Label(self, text="", foreground="#a00")
        self.msg.grid(row=5, column=0, columnspan=5, padx=10, pady=(0, 6), sticky="w")

    # --- input handling ---

    def voltage_for(self, pressure):
        """Command voltage for a pressure setpoint.

        Mirrors the firmware's P command (linear between the CAL PRESS
        endpoints), so the voltage shown on a preset is the one the firmware
        will actually program. If the calibration is changed there (CAL
        PRESS / CAL SAVE), the REGULATORS table here has to move with it.
        """
        span = self.p_at_10v - self.p_at_0v
        return VOLT_MIN + (pressure - self.p_at_0v) * (VOLT_MAX - VOLT_MIN) / span

    def warn(self, text=""):
        self.msg.config(text=text)

    def clamp(self, value, lo, hi, unit):
        """Hold a value inside its range, saying so rather than silently moving it."""
        if value < lo:
            self.warn(f"{value:g} is below range — clamped to {lo:g} {unit}")
            return lo
        if value > hi:
            self.warn(f"{value:g} is above range — clamped to {hi:g} {unit}")
            return hi
        self.warn()
        return value

    def apply_pressure(self, value):
        value = self.clamp(value, self.p_lo, self.p_hi, self.unit)
        self.p_entry.delete(0, "end")
        self.p_entry.insert(0, f"{value:g}")
        self.on_set_pressure(self.channel, value)

    def apply_pressure_entry(self):
        value = to_float(self.p_entry.get())
        if value is None:
            self.warn(f"enter a number between {self.p_lo:g} and {self.p_hi:g} {self.unit}")
            return
        self.apply_pressure(value)

    def apply_voltage_entry(self):
        value = to_float(self.v_entry.get())
        if value is None:
            self.warn(f"enter a number between {VOLT_MIN:g} and {VOLT_MAX:g} V")
            return
        value = self.clamp(value, VOLT_MIN, VOLT_MAX, "V")
        self.v_entry.delete(0, "end")
        self.v_entry.insert(0, f"{value:.3f}")
        self.on_set_voltage(self.channel, value)

    def set_enabled(self, enabled):
        """Lock the manual controls while a profile is driving this channel."""
        state = "normal" if enabled else "disabled"
        for widget in self.controls:
            widget.config(state=state)

    def clear(self):
        """Drive this one regulator to zero and reset its entries.

        Sends the same 0 kPa setpoint ZERO ALL uses, so the firmware clamps it
        to the safe end of this channel's range (1 kPa for air, -1.3 kPa for
        the vacuum units) instead of the GUI reporting an out-of-range value.
        """
        self.reset_fields()
        self.on_set_pressure(self.channel, 0.0)

    def reset_fields(self):
        self.p_entry.delete(0, "end")
        self.p_entry.insert(0, "0.0")
        self.v_entry.delete(0, "end")
        self.v_entry.insert(0, "0.000")
        self.warn()

    def show_readback(self, pressure, volts, monitor):
        """Fill the readback from one GET field triple (strings as reported)."""
        if monitor != "—":
            monitor = monitor_text(monitor, self.p_at_0v, self.p_at_10v, self.unit)
        self.readback.config(
            text=(f"commanded    {pressure} {self.unit}\n"
                  f"valve        {volts} V\n"
                  f"monitor      {monitor}"))


class ProfileRunner:
    """Steps a profile through the firmware without blocking the UI.

    Driven from Tk's after() queue rather than a thread or a sleep loop, so a
    multi-hour run stays responsive, Stop takes effect within one tick, and
    there is no second thread contending for the serial port.

    Holds are timed against time.monotonic() deadlines rather than by counting
    ticks, so a slow redraw or a busy machine cannot make a long soak drift.
    """

    TICK_MS = 250

    def __init__(self, app, profile):
        self.app = app
        self.profile = profile
        self.steps = profile["steps"]
        self.loop = 1
        self.index = -1
        self.deadline = 0.0
        self.job = None
        self.running = False

    def start(self):
        self.running = True
        self.app.log_line(
            f"# profile '{self.profile['name']}' started — "
            f"{len(self.steps)} steps x {self.profile['loops']} "
            f"= {fmt_hms(self.profile['duration_s'])}")
        self._next_step()

    def stop(self, reason):
        if not self.running:
            return
        self.running = False
        if self.job is not None:
            self.app.after_cancel(self.job)
            self.job = None
        self.app.log_line(f"# profile {reason}")
        if self.profile["zero_on_finish"]:
            self.app.zero_all()
        self.app.on_profile_finished()

    def _next_step(self):
        self.index += 1
        if self.index >= len(self.steps):
            if self.loop >= self.profile["loops"]:
                self.stop("finished")
                return
            self.loop += 1
            self.index = 0

        step = self.steps[self.index]
        self.app.log_line(
            f"# loop {self.loop}/{self.profile['loops']} "
            f"step {self.index + 1}/{len(self.steps)} "
            f"'{step['label']}' — hold {fmt_hms(step['hold_s'])}")
        for kind, channel, value in step["actions"]:
            if kind == "P":
                self.app.set_pressure(channel, value)
            else:
                self.app.set_voltage(channel, value)

        self.deadline = time.monotonic() + step["hold_s"]
        self._tick()

    def _tick(self):
        if not self.running:
            return
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            self._next_step()
            return
        step = self.steps[self.index]
        self.app.show_profile_progress(
            f"{self.profile['name']}  —  loop {self.loop}/{self.profile['loops']},"
            f"  step {self.index + 1}/{len(self.steps)} '{step['label']}',"
            f"  {fmt_hms(remaining)} left")
        self.job = self.app.after(self.TICK_MS, self._tick)


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("SRFM Regulator Control")
        self.resizable(False, False)
        # One column per regulator; every other widget spans the full width.
        span = NUM_CHANNELS
        for column in range(span):
            self.columnconfigure(column, weight=1, uniform="panel")

        self.rx_queue = queue.Queue()
        self.link = SerialLink(self.rx_queue.put)
        self.runner = None
        self._refresh_job = None
        self._hb_job = None
        self._hb_pending = 0  # HB acks still to arrive (and be dropped from the log)

        # --- connection bar ---
        bar = ttk.Frame(self)
        bar.grid(row=0, column=0, columnspan=span, sticky="ew", padx=8, pady=8)
        ttk.Label(bar, text="Port:").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(bar, textvariable=self.port_var, width=26)
        self.port_combo.pack(side="left", padx=4)
        ttk.Button(bar, text="⟳", width=3, command=self.refresh_ports).pack(side="left")
        self.connect_btn = ttk.Button(bar, text="Connect", command=self.toggle_connect)
        self.connect_btn.pack(side="left", padx=6)
        self.status = ttk.Label(bar, text="disconnected", foreground="#a00")
        self.status.pack(side="left", padx=6)
        # Board state: the latest !READY / !FAULT event, or the state named in
        # an ID / STATUS reply.
        self.board = ttk.Label(bar, text="", foreground="#444")
        self.board.pack(side="left", padx=6)

        # --- regulator panels, one per column, in channel order ---
        self.panels = {}
        for i, (fw_name, name, p_at_0v, p_at_10v, unit, presets) in enumerate(REGULATORS):
            panel = RegulatorPanel(self, i + 1, name, p_at_0v, p_at_10v, unit, presets,
                                   self.set_pressure, self.set_voltage)
            panel.grid(row=1, column=i, padx=(8 if i == 0 else 4, 8), pady=4,
                       sticky="nsew")
            self.panels[fw_name] = panel

        # --- bottom bar ---
        bottom = ttk.Frame(self)
        bottom.grid(row=2, column=0, columnspan=span, sticky="ew", padx=8, pady=(4, 8))
        ttk.Button(bottom, text="ZERO ALL", command=self.zero_all).pack(side="left")
        for label, command in (("GET", "GET"), ("VERIFY", "VERIFY"),
                               ("DUMP", "DUMP"), ("STATUS", "STATUS")):
            ttk.Button(bottom, text=label,
                       command=lambda c=command: self.send_cmd(c)).pack(
                side="left", padx=(6, 0))
        # Board power: STOP drops the 24 V rail, START brings it back,
        # CLEARFAULT is the only way out of a latched eFuse fault.
        ttk.Button(bottom, text="STOP", command=self.stop_board).pack(
            side="left", padx=(18, 0))
        ttk.Button(bottom, text="START",
                   command=lambda: self.send_cmd("START")).pack(side="left", padx=(6, 0))
        ttk.Button(bottom, text="CLEARFAULT",
                   command=lambda: self.send_cmd("CLEARFAULT")).pack(
            side="left", padx=(6, 0))

        # --- test profile bar ---
        script = ttk.LabelFrame(self, text=" Test profile ")
        script.grid(row=3, column=0, columnspan=span, sticky="ew", padx=8, pady=(0, 6))
        self.script_btn = ttk.Button(script, text="Script…", width=10,
                                     command=self.choose_profile)
        self.script_btn.pack(side="left", padx=(8, 4), pady=6)
        self.stop_btn = ttk.Button(script, text="Stop", width=8,
                                   state="disabled", command=self.stop_profile)
        self.stop_btn.pack(side="left", padx=4, pady=6)
        self.profile_label = ttk.Label(script, text="idle", foreground="#444")
        self.profile_label.pack(side="left", padx=8)

        self.log = tk.Text(self, height=8, width=78, state="disabled",
                           font="TkFixedFont")
        self.log.grid(row=4, column=0, columnspan=span, sticky="ew",
                      padx=8, pady=(0, 4))

        ttk.Label(self, text=COPYRIGHT, foreground="#777").grid(
            row=5, column=0, columnspan=span, padx=10, pady=(0, 6), sticky="e")

        self.refresh_ports()
        self.after(50, self.poll_rx)
        self.protocol("WM_DELETE_WINDOW", self.on_close)

    # --- connection handling ---

    def refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_combo["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    def toggle_connect(self):
        if self.link.connected:
            self.disconnect_link("port disconnected")
            return
        port = self.port_var.get().strip()
        if not port:
            self.log_line("! no port selected")
            return
        try:
            self.link.connect(port)
        except serial.SerialException as exc:
            self.log_line(f"! connect failed: {exc}")
            return
        self.status.config(text=f"connected {port}", foreground="#080")
        self.connect_btn.config(text="Disconnect")
        # The XIAO does not reset when the port opens; a short settle is
        # enough before identifying it and reading the current setpoints.
        self.after(CONNECT_SETTLE_MS, lambda: self.send_if_connected("ID"))
        self.after(CONNECT_SETTLE_MS + 200, lambda: self.send_if_connected("GET"))
        self._hb_pending = 0
        self._hb_job = self.after(HEARTBEAT_MS, self.heartbeat)

    def disconnect_link(self, reason):
        """Close the port, stopping whatever depends on it first."""
        self.stop_heartbeat()
        # Never leave a profile running against a port that is about to
        # close; it would keep ticking and log a send failure per step.
        if self.runner is not None:
            self.runner.stop(f"stopped — {reason}")
        self.link.disconnect()
        self.status.config(text="disconnected", foreground="#a00")
        self.board.config(text="", foreground="#444")
        self.connect_btn.config(text="Connect")

    def on_close(self):
        if self.runner is not None:
            self.runner.stop("stopped — window closed")
        self.stop_heartbeat()
        if self.link.connected:
            try:
                self.link.send("ZERO")
            except (RuntimeError, serial.SerialException):
                pass
            self.link.disconnect()
        self.destroy()

    # --- heartbeat ---

    def heartbeat(self):
        """Send HB and reschedule. Not logged: one line a second would bury
        everything else, so the HB and its bare OK are both kept out."""
        self._hb_job = None
        if not self.link.connected:
            return
        try:
            self.link.send("HB")
        except (RuntimeError, serial.SerialException) as exc:
            self.log_line(f"! heartbeat failed: {exc}")
            self.disconnect_link("port lost")
            return
        self._hb_pending += 1
        self._hb_job = self.after(HEARTBEAT_MS, self.heartbeat)

    def stop_heartbeat(self):
        if self._hb_job is not None:
            self.after_cancel(self._hb_job)
            self._hb_job = None
        self._hb_pending = 0

    # --- commands ---

    # Named send_cmd, not send: tk.Misc already defines a send() for Tk's
    # inter-interpreter messaging, and shadowing it is asking for trouble.
    def send_cmd(self, line):
        try:
            self.link.send(line)
            self.log_line(f"> {line}")
        except (RuntimeError, serial.SerialException) as exc:
            self.log_line(f"! send failed: {exc}")

    def send_if_connected(self, line):
        """For deferred sends: stay quiet if the port went away meanwhile."""
        if self.link.connected:
            self.send_cmd(line)

    def set_pressure(self, channel, value):
        self.send_cmd(f"P {channel} {value:.2f}")

    def set_voltage(self, channel, volts):
        self.send_cmd(f"V {channel} {volts:.3f}")

    def request_status(self, delay_ms=120):
        """Ask for a GET, coalescing bursts into one.

        A profile step that sets four regulators produces four acks; without
        this each would queue its own GET and the log would fill with
        identical status lines.
        """
        if self._refresh_job is not None:
            self.after_cancel(self._refresh_job)
        self._refresh_job = self.after(delay_ms, self._send_status_request)

    def _send_status_request(self):
        self._refresh_job = None
        self.send_cmd("GET")

    def zero_all(self):
        for panel in self.panels.values():
            panel.reset_fields()
        self.send_cmd("ZERO")
        self.request_status()

    def stop_board(self):
        """STOP: every output to zero, then the 24 V rail off.

        A running profile is ended first — its setpoints would only be
        refused with ERR once the board is STOPPED. START brings it back.
        """
        if self.runner is not None:
            self.runner.stop("stopped — board STOP")
        for panel in self.panels.values():
            panel.reset_fields()
        self.send_cmd("STOP")

    # --- test profiles ---

    def choose_profile(self):
        if self.runner is not None:
            return
        if not self.link.connected:
            messagebox.showwarning(
                "Not connected",
                "Connect to the controller before running a test profile.")
            return
        path = filedialog.askopenfilename(
            title="Select a test profile",
            initialdir=PROFILE_DIR if os.path.isdir(PROFILE_DIR) else None,
            filetypes=[("Test profile", "*.json"), ("All files", "*.*")])
        if not path:
            return
        try:
            profile = load_profile(path)
        except (OSError, ValueError) as exc:
            # Reject the whole profile rather than starting a long run that
            # would fail partway through.
            messagebox.showerror("Profile error", f"{os.path.basename(path)}\n\n{exc}")
            self.log_line(f"! profile rejected: {exc}")
            return
        self.start_profile(profile)

    def start_profile(self, profile):
        for panel in self.panels.values():
            panel.set_enabled(False)
        self.script_btn.config(state="disabled")
        self.stop_btn.config(state="normal")
        self.runner = ProfileRunner(self, profile)
        self.runner.start()

    def stop_profile(self):
        if self.runner is not None:
            self.runner.stop("stopped by user")

    def on_profile_finished(self):
        self.runner = None
        for panel in self.panels.values():
            panel.set_enabled(True)
        self.script_btn.config(state="normal")
        self.stop_btn.config(state="disabled")
        self.show_profile_progress("idle")

    def show_profile_progress(self, text):
        self.profile_label.config(text=text)

    # --- receive path ---

    def poll_rx(self):
        try:
            while True:
                self.handle_line(self.rx_queue.get_nowait())
        except queue.Empty:
            pass
        self.after(50, self.poll_rx)

    def handle_line(self, line):
        kind, payload = classify_reply(line)
        if kind == "ok" and self._hb_pending > 0:
            # The heartbeat's ack — replies arrive in command order, so the
            # next bare OK after an HB is its own. Dropped, not logged.
            self._hb_pending -= 1
            return
        self.log_line(f"< {line}")
        if kind == "event":
            self.show_event(line)
        elif kind == "ack":
            self.request_status()
        elif kind == "status":
            self.show_status(payload)
        else:
            state = reply_state(line)
            if state:
                self.show_state(state)

    def show_status(self, status):
        for name, (pressure, volts, monitor) in status.items():
            self.panels[name].show_readback(pressure, volts, monitor)

    def show_event(self, line):
        """Keep the latest !FAULT / !READY line in the bar; other events log only."""
        head = line.split()[0].upper() if line.split() else ""
        if head == "!FAULT":
            self.board.config(text=line, foreground="#a00")
        elif head == "!READY":
            self.board.config(text=line, foreground="#080")

    def show_state(self, state):
        colour = "#080" if state == "READY" else "#a00"
        self.board.config(text=f"state {state}", foreground=colour)

    def log_line(self, text):
        self.log.config(state="normal")
        self.log.insert("end", text + "\n")
        self.log.see("end")
        self.log.config(state="disabled")


if __name__ == "__main__":
    App().mainloop()
