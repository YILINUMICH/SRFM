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

# Display ranges for the sliders; must match the firmware/CAL calibration.
# (name, min, max, unit)
REGULATORS = [
    ("Air pressure (ITV0030)", 1.0, 500.0, "kPa"),
    ("Vacuum 1 (ITV2090)", -80.0, -1.3, "kPa"),
    ("Vacuum 2 (ITV2090)", -80.0, -1.3, "kPa"),
    ("Vacuum 3 (ITV2090)", -80.0, -1.3, "kPa"),
]


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
        if not self.connected:
            raise RuntimeError("not connected")
        self.ser.write((line + "\n").encode("ascii"))

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
    def __init__(self, master, index, name, pmin, pmax, unit, on_set):
        super().__init__(master, text=f"{index}: {name}")
        self.index = index
        self.on_set = on_set
        self.var = tk.DoubleVar(value=0.0)

        self.slider = ttk.Scale(
            self, from_=pmin, to=pmax, orient="horizontal",
            variable=self.var, length=260,
            command=lambda _v: self._sync_entry())
        self.slider.grid(row=0, column=0, columnspan=3, padx=8, pady=(6, 2), sticky="ew")
        # Apply only on release so dragging doesn't flood the serial port.
        self.slider.bind("<ButtonRelease-1>", lambda _e: self._apply())

        ttk.Label(self, text=f"{pmin:g} .. {pmax:g} {unit}").grid(
            row=1, column=0, padx=8, sticky="w")
        self.entry = ttk.Entry(self, width=8, justify="right")
        self.entry.grid(row=1, column=1, padx=4, pady=(0, 6), sticky="e")
        self.entry.insert(0, "0.0")
        self.entry.bind("<Return>", lambda _e: self._apply_from_entry())
        ttk.Button(self, text="Set", width=5, command=self._apply_from_entry).grid(
            row=1, column=2, padx=(0, 8), pady=(0, 6))

        self.readback = ttk.Label(self, text="—", foreground="#666")
        self.readback.grid(row=2, column=0, columnspan=3, padx=8, pady=(0, 6), sticky="w")
        self.columnconfigure(0, weight=1)

    def _sync_entry(self):
        self.entry.delete(0, "end")
        self.entry.insert(0, f"{self.var.get():.1f}")

    def _apply(self):
        self.on_set(self.index, self.var.get())

    def _apply_from_entry(self):
        try:
            value = float(self.entry.get())
        except ValueError:
            return
        self.var.set(value)
        self.on_set(self.index, value)

    def show_readback(self, text):
        self.readback.config(text=text)


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("SRFM Regulator Control")
        self.resizable(False, False)

        self.rx_queue = queue.Queue()
        self.link = SerialLink(self.rx_queue.put)

        # --- connection bar ---
        bar = ttk.Frame(self)
        bar.grid(row=0, column=0, columnspan=2, sticky="ew", padx=8, pady=8)
        ttk.Label(bar, text="Port:").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(bar, textvariable=self.port_var, width=28)
        self.port_combo.pack(side="left", padx=4)
        ttk.Button(bar, text="⟳", width=3, command=self.refresh_ports).pack(side="left")
        self.connect_btn = ttk.Button(bar, text="Connect", command=self.toggle_connect)
        self.connect_btn.pack(side="left", padx=6)
        self.status = ttk.Label(bar, text="disconnected", foreground="#a00")
        self.status.pack(side="left", padx=6)

        # --- regulator panels ---
        self.panels = []
        for i, (name, pmin, pmax, unit) in enumerate(REGULATORS):
            panel = RegulatorPanel(self, i, name, pmin, pmax, unit, self.set_pressure)
            panel.grid(row=1 + i // 2, column=i % 2, padx=8, pady=4, sticky="nsew")
            self.panels.append(panel)

        # --- bottom bar: zero-all + log ---
        bottom = ttk.Frame(self)
        bottom.grid(row=3, column=0, columnspan=2, sticky="ew", padx=8, pady=(4, 8))
        ttk.Button(bottom, text="ZERO ALL", command=self.zero_all).pack(side="left")
        ttk.Button(bottom, text="Refresh status", command=lambda: self.send("GET")).pack(
            side="left", padx=6)

        self.log = tk.Text(self, height=8, width=72, state="disabled",
                           font=("Menlo", 10))
        self.log.grid(row=4, column=0, columnspan=2, padx=8, pady=(0, 8))

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
        self.after(2500, lambda: self.send("ID"))

    def on_close(self):
        if self.link.connected:
            try:
                self.link.send("ZERO")
            except (RuntimeError, serial.SerialException):
                pass
            self.link.disconnect()
        self.destroy()

    # --- commands ---

    def send(self, line):
        try:
            self.link.send(line)
            self.log_line(f"> {line}")
        except (RuntimeError, serial.SerialException) as exc:
            self.log_line(f"! send failed: {exc}")

    def set_pressure(self, index, value):
        self.send(f"P {index} {value:.2f}")

    def zero_all(self):
        for panel in self.panels:
            panel.var.set(0.0)
            panel._sync_entry()
        self.send("ZERO")

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
        # "OK AIR=12.00,1.200V; VAC1=..." from GET, or "OK AIR p=12.00 v=1.200"
        if not line.startswith("OK "):
            return
        body = line[3:]
        if "=" in body and ";" in body:
            for i, part in enumerate(body.split(";")):
                part = part.strip()
                if "=" in part and i < len(self.panels):
                    self.panels[i].show_readback(part)
        elif " p=" in body and " v=" in body:
            # Single-regulator ack; pull a full status so readbacks stay fresh.
            self.send("GET")

    def log_line(self, text):
        self.log.config(state="normal")
        self.log.insert("end", text + "\n")
        self.log.see("end")
        self.log.config(state="disabled")


if __name__ == "__main__":
    App().mainloop()
