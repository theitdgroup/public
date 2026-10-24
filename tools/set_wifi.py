"""Set the watch's Wi-Fi over the USB cable (no hotspot needed).

    python tools/set_wifi.py            (asks for network name and password)
    python tools/set_wifi.py --port COM4

Needs pyserial (installed with PlatformIO).
"""
import argparse
import getpass
import json
import time

import serial
import serial.tools.list_ports


def find_port():
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x303A:  # Espressif native USB
            return p.device
    raise SystemExit("Watch not found on USB. Plug it in or pass --port COMx.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--ssid", help="Wi-Fi network name")
    args = ap.parse_args()
    port = args.port or find_port()

    ssid = (args.ssid or input("Wi-Fi network name: ")).strip()
    password = getpass.getpass("Wi-Fi password (hidden, empty for open network): ")
    if not ssid:
        raise SystemExit("No network name given.")

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.5
    # DTR+RTS together = "terminal ready": the watch only answers once both are set,
    # and this combination never triggers a reset into the bootloader
    s.dtr = s.rts = True
    s.open()
    with s:
        s.reset_input_buffer()
        s.write(b"cfg " + json.dumps({"wifi": [{"ssid": ssid, "pass": password}]}).encode() + b"\n")
        deadline = time.time() + 5
        while time.time() < deadline:
            line = s.readline().decode(errors="replace").strip()
            if line.startswith("OK"):
                break
            if line.startswith("ERR"):
                raise SystemExit("Watch rejected the settings: " + line)
        else:
            raise SystemExit("No answer from the watch. Is the ClockHub firmware running?")
        print("Saved. Waiting for the watch to join", ssid, "...")
        deadline = time.time() + 45
        while time.time() < deadline:
            s.write(b"status\n")
            end = time.time() + 2
            while time.time() < end:
                line = s.readline().decode(errors="replace").strip()
                if line.startswith("STATUS") and "connected=1" in line:
                    ip = line.split("ip=")[1]
                    print(f"\nConnected! Open http://{ip}/  (or http://clockhub.local/)")
                    return
        print("\nThe watch hasn't connected yet. Check the password; it keeps retrying in the background.")


if __name__ == "__main__":
    main()
