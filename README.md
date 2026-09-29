# Open Opal for Linux

Control the Opal C1 webcam on Linux: lock or trigger focus, set white balance and exposure,
and tune noise reduction and sharpening. All image processing runs on the camera's Intel
Myriad X chip, via [DepthAI](https://github.com/luxonis/depthai-core).

The sensor captures 4K, and the chip scales it down to 1080p, which cuts noise. The app
then sends that stream to a `v4l2loopback` virtual camera that Chrome, Meet, Zoom and
others can use.

Based on the ideas in [cansik/open-opal](https://github.com/cansik/open-opal). Not affiliated
with Opal Camera.

## One-time system setup

1. **USB access for DepthAI** (the Opal enumerates as Movidius `03e7`):
   ```sh
   echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="03e7", MODE="0666"' | sudo tee /etc/udev/rules.d/80-movidius.rules
   sudo udevadm control --reload-rules && sudo udevadm trigger
   ```
2. **Virtual camera** (`v4l2loopback-dkms` package):
   ```sh
   echo v4l2loopback | sudo tee /etc/modules-load.d/v4l2loopback.conf
   echo 'options v4l2loopback exclusive_caps=1 card_label="Opal C1 (Open Opal)" video_nr=10' | sudo tee /etc/modprobe.d/v4l2loopback.conf
   sudo modprobe v4l2loopback
   ```
   `exclusive_caps=1` is what makes Chrome list it.
3. **Optional: turn off USB power saving for the camera.** The Myriad X can misbehave in
   USB3 low-power link states:
   ```sh
   sudo cp packaging/open-opal-usb.conf /etc/tmpfiles.d/
   sudo systemd-tmpfiles --create /etc/tmpfiles.d/open-opal-usb.conf
   ```

## Install and run

```sh
./install.sh            # venv + editable install + menu entry
.venv/bin/open-opal     # or "Open Opal" in the app menu
```

In Chrome or your meeting app, pick **Opal C1 (Open Opal)**, not the plain "Opal C1".
Start Open Opal first: with `exclusive_caps=1` the virtual camera only appears once it
receives frames. If the camera doesn't show up, reload the tab.

Settings save automatically to `~/.config/open-opal-linux/settings.json`.

## Controls

- **Focus**: continuous autofocus, a fixed lens position (0–255), or *Focus once, then
  lock*, which runs autofocus once and freezes the result. Locking avoids focus hunting.
- **White balance**
  - *Auto (camera)*: the chip's own auto white balance.
  - *Auto + warmth bias*: a host-side loop adjusts the manual white balance a few times
    per second, so it follows changing light. *Warmth* sets the target: 0 is neutral grey,
    higher values are warmer. It helps under warm lamps, where the chip's own auto white
    balance leaves skin too red.
  - *Manual*: a fixed colour temperature. *Lock current* freezes whatever auto chose.
  - Lower K values give a bluer/cooler image. The ISP barely responds below ~2000 K.
- **Exposure**: auto with compensation (−9…+9), or manual shutter and ISO (up to 3200).
  *Meter on face* runs a small face detector (YuNet) on the camera and aims auto exposure
  at your face, and continuous autofocus too when that's on. It costs about 1% of a CPU
  core. The model downloads from the Luxonis model zoo on first run, then stays cached. Shutter times
  above 33 ms would drop below 30 fps, so the slider stops there. Set anti-flicker to
  match your mains frequency (50 Hz in Europe).
- **Image**: brightness, contrast, saturation, sharpness, and noise reduction for
  brightness and colour, all applied on the chip. *Tuning* picks the image tuning loaded
  onto the camera. *Low light* (the default) is Luxonis' low-light file, which `install.sh`
  downloads: it gives a brighter, much more neutral image in dim rooms. Changing tuning
  restarts the camera, which takes about 20 s.

## Notes

- While Open Opal runs, it has exclusive use of the camera, and the regular Opal webcam
  device goes away. It returns when the app quits.
- DepthAI warns `Calibration data not found`, because the Opal has no Luxonis lens
  calibration. It's harmless and has nothing to do with image tuning.
- After you quit, the normal Opal webcam takes about 15 s to come back. If you start Open
  Opal again before then, it waits for the camera to settle rather than failing.
