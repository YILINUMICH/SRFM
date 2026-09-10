"""PlatformIO upload hook for the SRFM XIAO nRF52840 board.

Before adafruit-nrfutil runs, get the board into its bootloader:
  * if a bootloader port (Seeed VID 0x2886, PID 0x0044/0x0045) is already
    present, use it (double-tap reset, or a previous interrupted upload);
  * otherwise open the application port (PID 0x8044/0x8045), send the DFU
    command, and wait for the bootloader port to appear.
The firmware's DFU path takes two resets (~3 s) because of the watchdog;
see src/main.cpp for why the stock 1200-baud touch is not used.
"""
import time

Import("env")  # noqa: F821  (PlatformIO SCons context)

VID = "2886"
BOOT_PIDS = ("0044", "0045")
APP_PIDS = ("8044", "8045")


def _ports():
    from serial.tools import list_ports
    found = []
    for p in list_ports.comports():
        hw = (p.hwid or "").upper()
        if "VID:PID=%s:" % VID not in hw:
            continue
        pid = hw.split("VID:PID=%s:" % VID, 1)[1][:4]
        found.append((p.device, pid))
    return found


def _wait_for_bootloader(timeout_s):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        for dev, pid in _ports():
            if pid in BOOT_PIDS:
                return dev
        time.sleep(0.2)
    return None


def enter_bootloader(source, target, env):
    import serial

    forced = env.subst("$UPLOAD_PORT")
    boot = _wait_for_bootloader(0)
    if boot:
        print("upload_dfu: bootloader already on %s" % boot)
        env.Replace(UPLOAD_PORT=boot)
        return

    apps = [dev for dev, pid in _ports() if pid in APP_PIDS]
    if forced and forced in apps:
        apps = [forced]
    if not apps:
        print("upload_dfu: no SRFM board found (VID 0x2886); leaving port as-is")
        return

    app = apps[0]
    print("upload_dfu: asking firmware on %s to enter the bootloader" % app)
    try:
        with serial.Serial(app, 115200, timeout=0.5) as s:
            time.sleep(0.2)
            s.write(b"DFU\n")
            time.sleep(0.2)
    except serial.SerialException as exc:
        print("upload_dfu: could not talk to %s (%s); trying anyway" % (app, exc))

    boot = _wait_for_bootloader(15)
    if boot:
        print("upload_dfu: bootloader on %s" % boot)
        env.Replace(UPLOAD_PORT=boot)
    else:
        print("upload_dfu: bootloader port did not appear; double-tap the reset "
              "button and run the upload again")


env.AddPreAction("upload", enter_bootloader)
