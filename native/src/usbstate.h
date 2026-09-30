// Which firmware the Opal C1 is running, read from sysfs (no USB access needed).
//
// The camera re-enumerates with a different product ID per firmware:
//   f63d  Opal's webcam firmware - the only state DepthAI can connect from reliably
//   f63b  DepthAI firmware - someone holds it, or it is still being released
//   other Myriad X bootloader, or mid-reboot
// Absent means it is off the bus, which happens for a few seconds during each switch.
#pragma once

enum class OpalState { Camera, DepthAI, Bootloader, Absent };

OpalState opalState();
