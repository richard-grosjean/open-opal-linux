from dataclasses import fields, replace

import numpy as np
from PySide6.QtCore import QEvent, QObject, Qt, QTimer, Signal
from PySide6.QtGui import QImage, QPixmap
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSlider,
    QVBoxLayout,
    QWidget,
)

from .camera import CameraWorker, Stats
from .settings import WB_AUTO, WB_AUTO_BIASED, WB_MANUAL, Settings

WB_MODES = [
    (WB_AUTO, "Auto (camera)"),
    (WB_AUTO_BIASED, "Auto + warmth bias"),
    (WB_MANUAL, "Manual"),
]
TUNINGS = [
    ("low_light", "Low light (Luxonis)"),
    ("default", "DepthAI default"),
]
ANTI_BANDING = [
    ("MAINS_50_HZ", "50 Hz"),
    ("MAINS_60_HZ", "60 Hz"),
    ("AUTO", "Auto"),
    ("OFF", "Off"),
]


class Bridge(QObject):
    """Carries callbacks from the camera thread onto the Qt thread."""

    preview = Signal(object)
    stats = Signal(object)
    status = Signal(str)
    lens_locked = Signal(int)


class SliderRow(QWidget):
    changed = Signal(int)

    def __init__(self, lo: int, hi: int, step: int = 1, fmt=str):
        super().__init__()
        self._fmt = fmt
        self.slider = QSlider(Qt.Orientation.Horizontal)
        self.slider.setRange(lo, hi)
        self.slider.setSingleStep(step)
        self.slider.setPageStep(step * 10)
        self.label = QLabel()
        self.label.setMinimumWidth(64)
        self.label.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.slider, 1)
        layout.addWidget(self.label)
        self.slider.valueChanged.connect(self._on_change)

    def _on_change(self, value: int) -> None:
        self.label.setText(self._fmt(value))
        self.changed.emit(value)

    def value(self) -> int:
        return self.slider.value()

    def set_value(self, value: int, quiet: bool = False) -> None:
        if quiet:
            self.slider.blockSignals(True)
        self.slider.setValue(value)
        self.label.setText(self._fmt(self.slider.value()))
        self.slider.blockSignals(False)


def signed(v: int) -> str:
    return f"{v:+d}" if v else "0"


class MainWindow(QMainWindow):
    def __init__(self, output_device: str | None = None):
        super().__init__()
        self.setWindowTitle("Open Opal")
        self.settings = Settings.load()

        self.bridge = Bridge()
        self.bridge.preview.connect(self._show_preview)
        self.bridge.stats.connect(self._show_stats)
        self.bridge.status.connect(self._show_status)
        self.bridge.lens_locked.connect(self._on_lens_locked)

        self._save_timer = QTimer(self)
        self._save_timer.setSingleShot(True)
        self._save_timer.setInterval(800)
        self._save_timer.timeout.connect(self._save)

        # Start connecting first: the camera takes seconds to boot, the window milliseconds.
        # Signals are queued until the event loop runs, by which time the widgets exist.
        self.worker = CameraWorker(
            self.settings,
            on_preview=self.bridge.preview.emit,
            on_stats=self.bridge.stats.emit,
            on_status=self.bridge.status.emit,
            on_lens_locked=self.bridge.lens_locked.emit,
            output_device=output_device,
        )
        self.worker.start()

        self._loading = True  # widgets fire change signals while being built
        self._build_ui()
        self._load_into_widgets(self.settings)

    # --- layout -----------------------------------------------------------------------

    def _build_ui(self) -> None:
        self.preview = QLabel("Starting camera…")
        self.preview.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.preview.setMinimumSize(480, 270)
        self.preview.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        self.preview.setStyleSheet("background: #111; color: #aaa;")

        self.info = QLabel()
        self.info.setStyleSheet("font-family: monospace;")

        left = QVBoxLayout()
        left.addWidget(self.preview, 1)
        left.addWidget(self.info)

        controls = QVBoxLayout()
        controls.addWidget(self._focus_group())
        controls.addWidget(self._wb_group())
        controls.addWidget(self._exposure_group())
        controls.addWidget(self._image_group())
        controls.addWidget(self._output_group())
        controls.addStretch(1)

        panel = QWidget()
        panel.setLayout(controls)
        scroll = QScrollArea()
        scroll.setWidget(panel)
        scroll.setWidgetResizable(True)
        scroll.setMinimumWidth(400)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)

        root = QHBoxLayout()
        root.addLayout(left, 1)
        root.addWidget(scroll)
        central = QWidget()
        central.setLayout(root)
        self.setCentralWidget(central)
        self.resize(1200, 720)

    def _focus_group(self) -> QGroupBox:
        box = QGroupBox("Focus")
        form = QFormLayout(box)
        self.auto_focus = QCheckBox("Continuous autofocus")
        self.lens = SliderRow(0, 255)
        self.focus_once = QPushButton("Focus once, then lock")
        form.addRow(self.auto_focus)
        form.addRow("Lens position", self.lens)
        form.addRow(self.focus_once)

        self.auto_focus.toggled.connect(lambda v: self._set(auto_focus=v))
        self.lens.changed.connect(lambda v: self._set(lens_position=v))
        self.focus_once.clicked.connect(self._on_focus_once)
        return box

    def _wb_group(self) -> QGroupBox:
        box = QGroupBox("White balance")
        form = QFormLayout(box)
        self.wb_mode = QComboBox()
        for key, label in WB_MODES:
            self.wb_mode.addItem(label, key)
        self.wb_warmth = SliderRow(-10, 10, fmt=signed)
        self.wb_kelvin = SliderRow(1000, 12000, step=50, fmt=lambda v: f"{v} K")
        self.wb_lock = QPushButton("Lock current")
        form.addRow("Mode", self.wb_mode)
        form.addRow("Warmth", self.wb_warmth)
        form.addRow("Temperature", self.wb_kelvin)
        form.addRow(self.wb_lock)

        self.wb_mode.currentIndexChanged.connect(lambda _: self._set(wb_mode=self.wb_mode.currentData()))
        self.wb_warmth.changed.connect(lambda v: self._set(wb_warmth=v))
        self.wb_kelvin.changed.connect(lambda v: self._set(wb_kelvin=v))
        self.wb_lock.clicked.connect(self._on_wb_lock)
        return box

    def _exposure_group(self) -> QGroupBox:
        box = QGroupBox("Exposure")
        form = QFormLayout(box)
        self.auto_exposure = QCheckBox("Auto exposure")
        self.exposure_comp = SliderRow(-9, 9, fmt=signed)
        self.exposure_us = SliderRow(100, 33000, step=100, fmt=lambda v: f"{v / 1000:.1f} ms")
        self.iso = SliderRow(100, 1600, step=50)
        self.anti_banding = QComboBox()
        for key, label in ANTI_BANDING:
            self.anti_banding.addItem(label, key)
        form.addRow(self.auto_exposure)
        form.addRow("Compensation", self.exposure_comp)
        form.addRow("Shutter", self.exposure_us)
        form.addRow("ISO", self.iso)
        form.addRow("Anti-flicker", self.anti_banding)

        self.auto_exposure.toggled.connect(lambda v: self._set(auto_exposure=v))
        self.exposure_comp.changed.connect(lambda v: self._set(exposure_compensation=v))
        self.exposure_us.changed.connect(lambda v: self._set(exposure_us=v))
        self.iso.changed.connect(lambda v: self._set(iso=v))
        self.anti_banding.currentIndexChanged.connect(
            lambda _: self._set(anti_banding=self.anti_banding.currentData())
        )
        return box

    def _image_group(self) -> QGroupBox:
        box = QGroupBox("Image")
        form = QFormLayout(box)
        self.tuning = QComboBox()
        for key, label in TUNINGS:
            self.tuning.addItem(label, key)
        self.tuning.setToolTip("Colour and exposure tuning loaded onto the camera. Changing it restarts the camera (about 20 s).")
        form.addRow("Tuning", self.tuning)
        self.tuning.currentIndexChanged.connect(lambda _: self._set(tuning=self.tuning.currentData()))
        self.image_sliders = {
            "brightness": SliderRow(-10, 10, fmt=signed),
            "contrast": SliderRow(-10, 10, fmt=signed),
            "saturation": SliderRow(-10, 10, fmt=signed),
            "sharpness": SliderRow(0, 4),
            "luma_denoise": SliderRow(0, 4),
            "chroma_denoise": SliderRow(0, 4),
        }
        labels = {
            "brightness": "Brightness",
            "contrast": "Contrast",
            "saturation": "Saturation",
            "sharpness": "Sharpness",
            "luma_denoise": "Noise reduction",
            "chroma_denoise": "Colour noise reduction",
        }
        for key, row in self.image_sliders.items():
            form.addRow(labels[key], row)
            row.changed.connect(lambda v, k=key: self._set(**{k: v}))
        return box

    def _output_group(self) -> QGroupBox:
        box = QGroupBox("Output")
        layout = QVBoxLayout(box)
        self.output_enabled = QCheckBox("Send to virtual camera (v4l2loopback)")
        reset = QPushButton("Reset all to defaults")
        layout.addWidget(self.output_enabled)
        layout.addWidget(reset)
        self.output_enabled.toggled.connect(lambda v: self._set(output_enabled=v))
        reset.clicked.connect(self._on_reset)
        return box

    # --- state ------------------------------------------------------------------------

    def _load_into_widgets(self, s: Settings) -> None:
        self._loading = True
        self.auto_focus.setChecked(s.auto_focus)
        self.lens.set_value(s.lens_position)
        self.wb_mode.setCurrentIndex(self.wb_mode.findData(s.wb_mode))
        self.wb_warmth.set_value(s.wb_warmth)
        self.wb_kelvin.set_value(s.wb_kelvin)
        self.auto_exposure.setChecked(s.auto_exposure)
        self.exposure_comp.set_value(s.exposure_compensation)
        self.exposure_us.set_value(s.exposure_us)
        self.iso.set_value(s.iso)
        self.anti_banding.setCurrentIndex(self.anti_banding.findData(s.anti_banding))
        self.tuning.setCurrentIndex(max(0, self.tuning.findData(s.tuning)))
        for key, row in self.image_sliders.items():
            row.set_value(getattr(s, key))
        self.output_enabled.setChecked(s.output_enabled)
        self._loading = False
        self._update_enabled()

    def _update_enabled(self) -> None:
        s = self.settings
        self.lens.setEnabled(not s.auto_focus)
        self.wb_warmth.setEnabled(s.wb_mode == WB_AUTO_BIASED)
        self.wb_kelvin.setEnabled(s.wb_mode == WB_MANUAL)
        self.wb_lock.setEnabled(s.wb_mode != WB_MANUAL)
        self.exposure_comp.setEnabled(s.auto_exposure)
        self.exposure_us.setEnabled(not s.auto_exposure)
        self.iso.setEnabled(not s.auto_exposure)

    def _set(self, **changes) -> None:
        if self._loading:
            return
        self.settings = replace(self.settings, **changes)
        self.worker.update(self.settings, set(changes))
        self._update_enabled()
        self._save_timer.start()

    def _save(self) -> None:
        self.settings.save()

    # --- actions ----------------------------------------------------------------------

    def _on_focus_once(self) -> None:
        self.auto_focus.setChecked(False)
        self.worker.focus_once()

    def _on_lens_locked(self, position: int) -> None:
        self.settings = replace(self.settings, auto_focus=False, lens_position=position)
        self.lens.set_value(position, quiet=True)
        self._update_enabled()
        self._save_timer.start()

    def _on_wb_lock(self) -> None:
        kelvin = self._last_stats.wb_kelvin if self._last_stats else self.settings.wb_kelvin
        self.wb_kelvin.set_value(kelvin, quiet=True)
        self._set(wb_mode=WB_MANUAL, wb_kelvin=kelvin)
        self.wb_mode.blockSignals(True)
        self.wb_mode.setCurrentIndex(self.wb_mode.findData(WB_MANUAL))
        self.wb_mode.blockSignals(False)

    def _on_reset(self) -> None:
        defaults = Settings()
        changed = {f.name for f in fields(Settings) if getattr(defaults, f.name) != getattr(self.settings, f.name)}
        self.settings = defaults
        self._load_into_widgets(defaults)
        self.worker.update(defaults, changed | {"auto_focus", "wb_mode", "auto_exposure"})
        self._save_timer.start()

    # --- camera feedback ----------------------------------------------------------------

    _last_stats: Stats | None = None

    def _show_preview(self, img: np.ndarray) -> None:
        h, w, _ = img.shape
        qimg = QImage(img.data, w, h, w * 3, QImage.Format.Format_BGR888)
        pix = QPixmap.fromImage(qimg).scaled(
            self.preview.size(), Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation
        )
        self.preview.setPixmap(pix)

    def _show_stats(self, st: Stats) -> None:
        self._last_stats = st
        self.info.setText(
            f"{st.fps:4.1f} fps   lens {st.lens_position:3d}   ISO {st.iso:4d}   "
            f"shutter {st.exposure_us / 1000:4.1f} ms   WB {st.wb_kelvin} K   → {st.output}"
        )
        # sliders follow what the camera is doing while it's in auto
        s = self.settings
        if s.auto_focus:
            self.lens.set_value(st.lens_position, quiet=True)
        if s.wb_mode != WB_MANUAL:
            self.wb_kelvin.set_value(st.wb_kelvin, quiet=True)
        if s.auto_exposure:
            self.exposure_us.set_value(st.exposure_us, quiet=True)
            self.iso.set_value(st.iso, quiet=True)

    def _show_status(self, text: str) -> None:
        self.statusBar().showMessage(text)
        if not text.startswith("Streaming"):
            self.preview.setText(text)

    def changeEvent(self, event) -> None:
        if event.type() == QEvent.Type.WindowStateChange:
            self.worker.set_preview_enabled(not self.isMinimized())
        super().changeEvent(event)

    def closeEvent(self, event) -> None:
        self._save()
        self.worker.stop()
        super().closeEvent(event)
