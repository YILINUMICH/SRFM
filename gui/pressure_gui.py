#!/usr/bin/env python3
"""SRFM regulator control GUI.

Talks to the Arduino Mega + LTC2668 firmware over USB serial
(115200 baud, line-based protocol — see src/main.cpp).

Requires: pyserial  (pip install -r requirements.txt)
"""

import queue
import threading
import tkinter as tk
from tkinter import ttk

import serial
import serial.tools.list_ports

BAUD = 115200

# DAC command-signal range. Must match the span the firmware selects in setup()
# (LTC2668_SPAN_0_TO_10V) and the 0-10V input both regulator models accept.
VOLT_MIN, VOLT_MAX = 0.0, 10.0

# (name, pressure at 0V, pressure at 10V, unit, preset buttons)
# The two pressures are in calibration order — pressure at vMin then at vMax,
# matching the table in lib/PressureControl/PressureControl.h. They are NOT
# sorted numerically: the vacuum units run from -1.3 kPa at 0V down to
# -80 kPa at 10V, and showing them in that order is what makes the range
# readable next to the DAC voltage.
REGULATORS = [
    ("Air pressure (ITV0030)", 1.0, 500.0, "kPa", (1, 50, 100, 200, 350, 500)),
    ("Vacuum 1 (ITV2090)", -1.3, -80.0, "kPa", (-1.3, -10, -20, -40, -60, -80)),
    ("Vacuum 2 (ITV2090)", -1.3, -80.0, "kPa", (-1.3, -10, -20, -40, -60, -80)),
    ("Vacuum 3 (ITV2090)", -1.3, -80.0, "kPa", (-1.3, -10, -20, -40, -60, -80)),
]


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


class SerialLink:
    """Background reader thread around a pyserial port."""

    def __init__(self, on_line):
        self.ser = None
        self.on_line = on_line
        self._thread = None

    @property
    def connected(self):
        return self.ser is not None and self.ser.is_open

    def connect(self, port):
        self.disconnect()
        self.ser = serial.Serial(port, BAUD, timeout=0.2)
        self._thread = threading.Thread(target=self._read_loop, daemon=True)
        self._thread.start()

    def disconnect(self):
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
        while ser is not None and ser.is_open:
            try:
                raw = ser.readline()
            except (serial.SerialException, TypeError, OSError):
                break
            if raw:
                self.on_line(raw.decode("ascii", errors="replace").strip())


class RegulatorPanel(ttk.LabelFrame):
    """One regulator: presets, manual pressure, manual voltage, readback."""

    def __init__(self, master, index, name, p_at_0v, p_at_10v, unit,
                 presets, on_set_pressure, on_set_voltage):
        super().__init__(master, text=f" {index}:  {name} ")
        self.index = index
        self.unit = unit
        self.p_at_0v = p_at_0v
        self.p_at_10v = p_at_10v
        self.p_lo = min(p_at_0v, p_at_10v)
        self.p_hi = max(p_at_0v, p_at_10v)
        self.on_set_pressure = on_set_pressure
        self.on_set_voltage = on_set_voltage

        vcmd = (self.register(is_partial_number), "%P")

        ttk.Label(
            self,
            text=(f"Range    {p_at_0v:g} … {p_at_10v:g} {unit}"
                  f"        DAC    {VOLT_MIN:g} … {VOLT_MAX:g} V"),
        ).grid(row=0, column=0, columnspan=4, padx=10, pady=(6, 4), sticky="w")

        # --- preset buttons: one click straight to a working setpoint ---
        ttk.Label(self, text="Presets").grid(
            row=1, column=0, padx=(10, 6), pady=2, sticky="w")
        preset_bar = ttk.Frame(self)
        preset_bar.grid(row=1, column=1, columnspan=3, padx=(0, 10), pady=2, sticky="w")
        for value in presets:
            ttk.Button(preset_bar,
                       text=f"{value:g} {unit}\n{self.voltage_for(value):.2f} V",
                       width=9,
                       command=lambda v=value: self.apply_pressure(v)).pack(
                           side="left", padx=2)

        # --- manual pressure entry ---
        ttk.Label(self, text="Pressure").grid(
            row=2, column=0, padx=(10, 6), pady=2, sticky="w")
        self.p_entry = ttk.Entry(self, width=10, justify="right",
                                 validate="key", validatecommand=vcmd)
        self.p_entry.insert(0, "0.0")
        self.p_entry.grid(row=2, column=1, pady=2, sticky="w")
        self.p_entry.bind("<Return>", lambda _e: self.apply_pressure_entry())
        ttk.Label(self, text=unit, width=4).grid(row=2, column=2, padx=(6, 0), sticky="w")
        ttk.Button(self, text="Set", width=7,
                   command=self.apply_pressure_entry).grid(
                       row=2, column=3, padx=(4, 10), sticky="w")

        # --- manual voltage entry (bypasses the pressure mapping) ---
        ttk.Label(self, text="Voltage").grid(
            row=3, column=0, padx=(10, 6), pady=2, sticky="w")
        self.v_entry = ttk.Entry(self, width=10, justify="right",
                                 validate="key", validatecommand=vcmd)
        self.v_entry.insert(0, "0.000")
        self.v_entry.grid(row=3, column=1, pady=2, sticky="w")
        self.v_entry.bind("<Return>", lambda _e: self.apply_voltage_entry())
        ttk.Label(self, text="V", width=4).grid(row=3, column=2, padx=(6, 0), sticky="w")
        ttk.Button(self, text="Set V", width=7,
                   command=self.apply_voltage_entry).grid(
                       row=3, column=3, padx=(4, 10), sticky="w")

        self.readback = ttk.Label(self, text="commanded    —", foreground="#444")
        self.readback.grid(row=4, column=0, columnspan=4, padx=10, pady=(6, 2),
                           sticky="w")
        self.msg = ttk.Label(self, text="", foreground="#a00")
        self.msg.grid(row=5, column=0, columnspan=4, padx=10, pady=(0, 6), sticky="w")

    # --- input handling ---

    def voltage_for(self, pressure):
        """Command voltage for a pressure setpoint.

        Mirrors setPressure() in lib/PressureControl/PressureControl.cpp, so
        the voltage shown on a preset is the one the firmware will actually
        program. If the calibration is changed there (or via CAL), the
        REGULATORS table here has to move with it.
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
        self.on_set_pressure(self.index, value)

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
        self.on_set_voltage(self.index, value)

    def reset_fields(self):
        self.p_entry.delete(0, "end")
        self.p_entry.insert(0, "0.0")
        self.v_entry.delete(0, "end")
        self.v_entry.insert(0, "0.000")
        self.warn()

    def show_readback(self, pressure, volts):
        self.readback.config(
            text=f"commanded    {pressure} {self.unit}        DAC    {volts} V")


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("SRFM Regulator Control")
        self.resizable(False, False)
        self.columnconfigure(0, weight=1)

        self.rx_queue = queue.Queue()
        self.link = SerialLink(self.rx_queue.put)

        # --- connection bar ---
        bar = ttk.Frame(self)
        bar.grid(row=0, column=0, sticky="ew", padx=8, pady=8)
        ttk.Label(bar, text="Port:").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(bar, textvariable=self.port_var, width=26)
        self.port_combo.pack(side="left", padx=4)
        ttk.Button(bar, text="⟳", width=3, command=self.refresh_ports).pack(side="left")
        self.connect_btn = ttk.Button(bar, text="Connect", command=self.toggle_connect)
        self.connect_btn.pack(side="left", padx=6)
        self.status = ttk.Label(bar, text="disconnected", foreground="#a00")
        self.status.pack(side="left", padx=6)

        # --- regulator panels, one per row ---
        self.panels = []
        for i, (name, p_at_0v, p_at_10v, unit, presets) in enumerate(REGULATORS):
            panel = RegulatorPanel(self, i, name, p_at_0v, p_at_10v, unit, presets,
                                   self.set_pressure, self.set_voltage)
            panel.grid(row=1 + i, column=0, padx=8, pady=4, sticky="ew")
            self.panels.append(panel)

        # --- bottom bar ---
        bottom = ttk.Frame(self)
        bottom.grid(row=1 + len(REGULATORS), column=0, sticky="ew", padx=8, pady=(4, 8))
        ttk.Button(bottom, text="ZERO ALL", command=self.zero_all).pack(side="left")
        ttk.Button(bottom, text="Refresh status",
                   command=lambda: self.send_cmd("GET")).pack(side="left", padx=6)
        ttk.Button(bottom, text="Verify SPI",
                   command=lambda: self.send_cmd("VERIFY")).pack(side="left")
        ttk.Button(bottom, text="Dump DAC",
                   command=lambda: self.send_cmd("DUMP")).pack(side="left", padx=6)

        self.log = tk.Text(self, height=7, width=78, state="disabled",
                           font="TkFixedFont")
        self.log.grid(row=2 + len(REGULATORS), column=0, padx=8, pady=(0, 8))

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
            self.link.disconnect()
            self.status.config(text="disconnected", foreground="#a00")
            self.connect_btn.config(text="Connect")
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
        # Opening the port resets the Mega; give it a moment then identify.
        self.after(2500, lambda: self.send_cmd("ID"))
        self.after(2700, lambda: self.send_cmd("GET"))

    def on_close(self):
        if self.link.connected:
            try:
                self.link.send("ZERO")
            except (RuntimeError, serial.SerialException):
                pass
            self.link.disconnect()
        self.destroy()

    # --- commands ---

    # Named send_cmd, not send: tk.Misc already defines a send() for Tk's
    # inter-interpreter messaging, and shadowing it is asking for trouble.
    def send_cmd(self, line):
        try:
            self.link.send(line)
            self.log_line(f"> {line}")
        except (RuntimeError, serial.SerialException) as exc:
            self.log_line(f"! send failed: {exc}")

    def set_pressure(self, index, value):
        self.send_cmd(f"P {index} {value:.2f}")

    def set_voltage(self, index, volts):
        # Regulator index equals its DAC channel (see the channel table in
        # README.md and the dacChannel field in PressureControl.h).
        self.send_cmd(f"V {index} {volts:.3f}")

    def zero_all(self):
        for panel in self.panels:
            panel.reset_fields()
        self.send_cmd("ZERO")
        self.after(80, lambda: self.send_cmd("GET"))

    # --- receive path ---

    def poll_rx(self):
        try:
            while True:
                line = self.rx_queue.get_nowait()
                self.log_line(f"< {line}")
                self.parse_status(line)
        except queue.Empty:
            pass
        self.after(50, self.poll_rx)

    def parse_status(self, line):
        if not line.startswith("OK "):
            return
        body = line[3:].strip()

        # DUMP output ("ch0=16384,2.500V,s1; ...") is for the log only.
        if body.startswith("ch0=") and ",s" in body:
            return

        # Single-command acks from P ("AIR p=.. v=..") and V/C ("ch0 v=.. code=..").
        # Pull a full status afterwards so every panel stays in step, including
        # the ones the command did not touch.
        if (" p=" in body and " v=" in body) or body.startswith("ch"):
            self.after(80, lambda: self.send_cmd("GET"))
            return

        # Full status: "AIR=250.50,5.000V; VAC1=-1.30,0.000V; ..."
        if "=" in body and "," in body:
            for i, part in enumerate(p.strip() for p in body.split(";")):
                if i >= len(self.panels) or "=" not in part:
                    continue
                _, _, rhs = part.partition("=")
                pressure, _, volts = rhs.partition(",")
                self.panels[i].show_readback(
                    pressure.strip(), volts.strip().rstrip("V"))

    def log_line(self, text):
        self.log.config(state="normal")
        self.log.insert("end", text + "\n")
        self.log.see("end")
        self.log.config(state="disabled")


if __name__ == "__main__":
    App().mainloop()
