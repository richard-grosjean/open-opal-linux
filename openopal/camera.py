"""Runs the DepthAI pipeline on the Opal C1 and applies settings to the on-chip ISP.

The sensor captures 4K; the Myriad X ISP denoises, sharpens, runs AE/AWB/AF and scales
to 1080p NV12 on the device. The host only forwards those frames to v4l2loopback, plus a
small BGR preview for the UI.
"""

import math
import queue
import threading
import time
from dataclasses import dataclass, replace
from datetime import timedelta
from typing import Callable, Optional

import depthai as dai
import numpy as np

from .settings import WB_AUTO, WB_AUTO_BIASED, WB_MANUAL, Settings
from .v4l2out import V4L2LoopbackOutput, find_loopback_device

OUTPUT_SIZE = (1920, 1080)
PREVIEW_SIZE = (640, 360)
PREVIEW_FPS = 30
SENSOR_SIZE = (3840, 2160)
FPS = 30

ONE_SHOT_FOCUS_SECONDS = 1.5
WB_LOOP_INTERVAL = 0.25
# the loop sleeps on the frame queue; this bounds how long a UI change waits without frames
FRAME_WAIT = timedelta(milliseconds=50)
# the ISP barely responds to manual white balance below ~2000 K
WB_LOOP_MIN_K = 2000.0
WB_LOOP_MAX_K = 10000.0


@dataclass
class Stats:
    fps: float = 0.0
    lens_position: int = 0
    iso: int = 0
    exposure_us: int = 0
    wb_kelvin: int = 0
    output: str = ""


def _ctrl() -> dai.CameraControl:
    return dai.CameraControl()


def build_controls(s: Settings, keys: Optional[set[str]] = None) -> list[dai.CameraControl]:
    """Translate settings into camera controls. With keys, only the affected groups."""

    def want(*names: str) -> bool:
        return keys is None or any(n in keys for n in names)

    controls = []

    if want("auto_focus", "lens_position"):
        c = _ctrl()
        if s.auto_focus:
            c.setAutoFocusMode(dai.CameraControl.AutoFocusMode.CONTINUOUS_VIDEO)
        else:
            c.setAutoFocusMode(dai.CameraControl.AutoFocusMode.OFF)
            c.setManualFocus(max(0, min(255, s.lens_position)))
        controls.append(c)

    if want("wb_mode", "wb_kelvin"):
        c = _ctrl()
        if s.wb_mode == WB_AUTO:
            c.setAutoWhiteBalanceMode(dai.CameraControl.AutoWhiteBalanceMode.AUTO)
            controls.append(c)
        elif s.wb_mode == WB_MANUAL:
            c.setManualWhiteBalance(max(1000, min(12000, s.wb_kelvin)))
            controls.append(c)
        # WB_AUTO_BIASED is driven by the feedback loop in CameraWorker

    if want("auto_exposure", "exposure_compensation", "exposure_us", "iso"):
        c = _ctrl()
        if s.auto_exposure:
            c.setAutoExposureEnable()
            c.setAutoExposureCompensation(max(-9, min(9, s.exposure_compensation)))
        else:
            c.setManualExposure(max(1, min(33000, s.exposure_us)), max(100, min(1600, s.iso)))
        controls.append(c)

    if want("anti_banding"):
        c = _ctrl()
        c.setAntiBandingMode(getattr(dai.CameraControl.AntiBandingMode, s.anti_banding))
        controls.append(c)

    if want("brightness", "contrast", "saturation", "sharpness", "luma_denoise", "chroma_denoise"):
        c = _ctrl()
        c.setBrightness(s.brightness)
        c.setContrast(s.contrast)
        c.setSaturation(s.saturation)
        c.setSharpness(s.sharpness)
        c.setLumaDenoise(s.luma_denoise)
        c.setChromaDenoise(s.chroma_denoise)
        controls.append(c)

    return controls


def red_blue_balance(bgr: np.ndarray) -> Optional[float]:
    """log(mean R / mean B) over mid-tone pixels; 0 means neutral (grey-world)."""
    px = bgr[::6, ::6].reshape(-1, 3).astype(np.float32)
    luma = px.mean(axis=1)
    mid = px[(luma > 25) & (luma < 225) & (px.max(axis=1) < 250)]
    if len(mid) < 200:
        return None
    b, _, r = mid.mean(axis=0)
    if b < 1 or r < 1:
        return None
    return math.log(r / b)


class CameraWorker:
    """Owns the device in a background thread; reconnects if the camera goes away."""

    def __init__(
        self,
        settings: Settings,
        on_preview: Callable[[np.ndarray], None],
        on_stats: Callable[[Stats], None],
        on_status: Callable[[str], None],
        on_lens_locked: Callable[[int], None],
        output_device: Optional[str] = None,
    ):
        self._settings = replace(settings)
        self._on_preview = on_preview
        self._on_stats = on_stats
        self._on_status = on_status
        self._on_lens_locked = on_lens_locked
        self._output_device = output_device

        self._pending: "queue.Queue[tuple[Settings, Optional[set[str]]]]" = queue.Queue()
        self._one_shot_focus = threading.Event()
        self._preview_enabled = True
        self._running = False
        self._thread: Optional[threading.Thread] = None

    # --- called from the UI thread -------------------------------------------------

    def start(self) -> None:
        self._running = True
        self._thread = threading.Thread(target=self._run, name="opal-camera", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._running = False
        if self._thread:
            self._thread.join(timeout=5)

    def update(self, settings: Settings, keys: Optional[set[str]] = None) -> None:
        self._pending.put((replace(settings), keys))

    def focus_once(self) -> None:
        self._one_shot_focus.set()

    def set_preview_enabled(self, enabled: bool) -> None:
        self._preview_enabled = enabled

    # --- camera thread ---------------------------------------------------------------

    def _run(self) -> None:
        while self._running:
            try:
                self._session()
            except Exception as e:  # device unplugged, busy, firmware hiccup, ...
                self._on_status(f"Camera error: {e}. Retrying…")
                time.sleep(2)

    def _open_output(self) -> tuple[Optional[V4L2LoopbackOutput], str]:
        if not self._settings.output_enabled:
            return None, "output off"
        device = self._output_device or find_loopback_device()
        if device is None:
            return None, "no v4l2loopback device"
        try:
            return V4L2LoopbackOutput(device, *OUTPUT_SIZE), device
        except OSError as e:
            return None, f"{device}: {e.strerror}"

    def _session(self) -> None:
        # Connecting reboots the camera out of Opal's firmware and uploads DepthAI's over USB
        self._on_status("Restarting camera… (takes about 7 s)")
        with dai.Pipeline() as pipeline:
            self._on_status("Starting stream…")
            cam = pipeline.create(dai.node.Camera).build(
                dai.CameraBoardSocket.CAM_A, sensorResolution=SENSOR_SIZE, sensorFps=FPS
            )
            main_q = cam.requestOutput(OUTPUT_SIZE, dai.ImgFrame.Type.NV12, fps=FPS).createOutputQueue(
                maxSize=2, blocking=False
            )
            preview_q = cam.requestOutput(PREVIEW_SIZE, dai.ImgFrame.Type.BGR888i, fps=PREVIEW_FPS).createOutputQueue(
                maxSize=2, blocking=False
            )
            control_q = cam.inputControl.createInputQueue()
            pipeline.start()

            s = self._settings
            for c in build_controls(s):
                control_q.send(c)

            output, output_label = self._open_output()
            self._on_status("Streaming")

            wb_kelvin = float(s.wb_kelvin)
            last_wb_step = 0.0
            focus_deadline = 0.0
            frames, fps_t0 = 0, time.monotonic()
            stats = Stats(output=output_label)

            try:
                while self._running and pipeline.isRunning():
                    # apply settings changes from the UI
                    while True:
                        try:
                            new, keys = self._pending.get_nowait()
                        except queue.Empty:
                            break
                        entering_biased = new.wb_mode == WB_AUTO_BIASED and s.wb_mode != WB_AUTO_BIASED
                        output_toggled = new.output_enabled != s.output_enabled
                        s = self._settings = new
                        for c in build_controls(s, keys):
                            control_q.send(c)
                        if entering_biased and stats.wb_kelvin:
                            wb_kelvin = max(WB_LOOP_MIN_K, min(WB_LOOP_MAX_K, float(stats.wb_kelvin)))  # start near the chip's AWB
                        if output_toggled:
                            if output:
                                output.close()
                            output, output_label = self._open_output()
                            stats.output = output_label

                    if self._one_shot_focus.is_set():
                        self._one_shot_focus.clear()
                        c = _ctrl()
                        c.setAutoFocusMode(dai.CameraControl.AutoFocusMode.AUTO)
                        c.setAutoFocusTrigger()
                        control_q.send(c)
                        focus_deadline = time.monotonic() + ONE_SHOT_FOCUS_SECONDS

                    frame = main_q.get(FRAME_WAIT)  # wakes once per frame instead of polling
                    if frame is not None:
                        if output:
                            try:
                                output.write(frame.getData())
                            except OSError as e:
                                output.close()
                                output, stats.output = None, f"output error: {e.strerror}"
                        frames += 1
                        stats.lens_position = frame.getLensPosition()
                        stats.iso = frame.getSensitivity()
                        stats.exposure_us = int(frame.getExposureTime().total_seconds() * 1e6)
                        stats.wb_kelvin = frame.getColorTemperature()

                    preview = preview_q.tryGet()
                    if preview is not None:
                        img = preview.getCvFrame()
                        if self._preview_enabled:
                            self._on_preview(img)
                        now = time.monotonic()
                        if s.wb_mode == WB_AUTO_BIASED and now - last_wb_step >= WB_LOOP_INTERVAL:
                            last_wb_step = now
                            wb_kelvin = self._wb_step(img, wb_kelvin, s.wb_warmth, control_q)

                    now = time.monotonic()
                    if focus_deadline and now >= focus_deadline:
                        focus_deadline = 0.0
                        s = self._settings = replace(s, auto_focus=False, lens_position=stats.lens_position)
                        for c in build_controls(s, {"lens_position"}):
                            control_q.send(c)
                        self._on_lens_locked(stats.lens_position)

                    if now - fps_t0 >= 1.0:
                        stats.fps = frames / (now - fps_t0)
                        frames, fps_t0 = 0, now
                        self._on_stats(replace(stats))

            finally:
                if output:
                    output.close()

    @staticmethod
    def _wb_step(img: np.ndarray, kelvin: float, warmth: int, control_q) -> float:
        """Nudge the manual WB so the frame's red/blue balance approaches the target.

        Telling the ISP the light is warmer (lower K) makes it add more blue, so a frame
        that is too red needs a lower K.
        """
        balance = red_blue_balance(img)
        if balance is None:
            return kelvin
        error = balance - warmth * 0.05
        if abs(error) < 0.015:
            return kelvin
        at_floor = kelvin <= WB_LOOP_MIN_K and error > 0
        at_ceiling = kelvin >= WB_LOOP_MAX_K and error < 0
        if at_floor or at_ceiling:
            return kelvin
        # log(R/B) moves ~2.6x as fast as log(K), so a gain of 0.2 settles without ringing
        step = max(-0.05, min(0.05, 0.2 * error))
        kelvin = max(WB_LOOP_MIN_K, min(WB_LOOP_MAX_K, kelvin * math.exp(-step)))
        c = _ctrl()
        c.setManualWhiteBalance(int(kelvin))
        control_q.send(c)
        return kelvin
