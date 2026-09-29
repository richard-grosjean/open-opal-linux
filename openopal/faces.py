"""Decode YuNet (OpenCV 2022, prior-box variant) outputs into the most confident face.

The network runs on the camera; only its three small tensors come back to the host.
Decoding follows OpenCV's face_detect.cpp for a 320x240 input.
"""

from typing import Optional

import numpy as np

INPUT_W, INPUT_H = 320, 240
STEPS = (8, 16, 32, 64)
MIN_SIZES = ((10, 16, 24), (32, 48), (64, 96), (128, 192, 256))
VARIANCE = (0.1, 0.2)
SCORE_THRESHOLD = 0.6


def _priors(w: int, h: int) -> np.ndarray:
    fm2 = ((h + 1) // 2 // 2, (w + 1) // 2 // 2)
    sizes = [(fm2[0] // 2, fm2[1] // 2)]
    for _ in range(3):
        sizes.append((sizes[-1][0] // 2, sizes[-1][1] // 2))
    out = []
    for (fh, fw), step, mins in zip(sizes, STEPS, MIN_SIZES):
        for i in range(fh):
            for j in range(fw):
                for m in mins:
                    out.append(((j + 0.5) * step / w, (i + 0.5) * step / h, m / w, m / h))
    return np.asarray(out, dtype=np.float32)


PRIORS = _priors(INPUT_W, INPUT_H)


def best_face(loc: np.ndarray, conf: np.ndarray, iou: np.ndarray) -> Optional[tuple[float, float, float, float, float]]:
    """(x, y, w, h, score) of the most confident face, normalised to 0..1, or None."""
    if loc.shape[0] != PRIORS.shape[0]:
        return None
    score = np.sqrt(conf[:, 1] * np.clip(iou[:, 0], 0.0, 1.0))
    i = int(np.argmax(score))
    if score[i] < SCORE_THRESHOLD:
        return None
    px, py, pw, ph = PRIORS[i]
    lx, ly, lw, lh = loc[i, :4]
    cx = px + lx * VARIANCE[0] * pw
    cy = py + ly * VARIANCE[0] * ph
    w = pw * np.exp(lw * VARIANCE[1])
    h = ph * np.exp(lh * VARIANCE[1])
    x0, y0 = max(0.0, cx - w / 2), max(0.0, cy - h / 2)
    x1, y1 = min(1.0, cx + w / 2), min(1.0, cy + h / 2)
    if x1 <= x0 or y1 <= y0:
        return None
    return float(x0), float(y0), float(x1 - x0), float(y1 - y0), float(score[i])
