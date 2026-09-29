"""Which firmware the Opal C1 is running, read from sysfs (no USB access needed).

The camera re-enumerates with a different product ID per firmware:
  f63d  Opal's webcam firmware - the only state DepthAI can connect from reliably
  f63b  DepthAI firmware - someone holds it, or it is still being released
  other Myriad X bootloader, or mid-reboot
Absent means it is off the bus, which happens for a few seconds during each switch.
"""

from pathlib import Path

VENDOR = "03e7"
PID_CAMERA = "f63d"
PID_DEPTHAI = "f63b"

CAMERA, DEPTHAI, BOOTLOADER, ABSENT = "camera", "depthai", "bootloader", "absent"


def opal_state() -> str:
    for dev in Path("/sys/bus/usb/devices").iterdir():
        try:
            if (dev / "idVendor").read_text().strip() != VENDOR:
                continue
            pid = (dev / "idProduct").read_text().strip()
        except OSError:
            continue
        if pid == PID_CAMERA:
            return CAMERA
        if pid == PID_DEPTHAI:
            return DEPTHAI
        return BOOTLOADER
    return ABSENT
