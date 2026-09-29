"""Minimal writer for a v4l2loopback device, without pyvirtualcam.

Sets the output format once with VIDIOC_S_FMT, then each frame is a plain write().
"""

import fcntl
import os
import struct

V4L2_BUF_TYPE_VIDEO_OUTPUT = 2
V4L2_FIELD_NONE = 1
V4L2_COLORSPACE_REC709 = 3

# sizeof(struct v4l2_format) is 208 on 64-bit Linux; _IOWR('V', 5, struct v4l2_format)
_V4L2_FORMAT_SIZE = 208
VIDIOC_S_FMT = (3 << 30) | (_V4L2_FORMAT_SIZE << 16) | (ord("V") << 8) | 5


def fourcc(code: str) -> int:
    a, b, c, d = (ord(ch) for ch in code)
    return a | (b << 8) | (c << 16) | (d << 24)


class V4L2LoopbackOutput:
    def __init__(self, device: str, width: int, height: int, pixel_format: str = "NV12"):
        if pixel_format != "NV12":
            raise ValueError("only NV12 is supported")
        self.device = device
        self.frame_size = width * height * 3 // 2
        self.fd = os.open(device, os.O_WRONLY)

        # struct v4l2_format: u32 type, 4 bytes padding (union is 8-aligned), then v4l2_pix_format
        pix = struct.pack(
            "12I",
            width,
            height,
            fourcc(pixel_format),
            V4L2_FIELD_NONE,
            width,  # bytesperline (luma plane)
            self.frame_size,
            V4L2_COLORSPACE_REC709,
            0, 0, 0, 0, 0,  # priv, flags, ycbcr_enc, quantization, xfer_func
        )
        buf = bytearray(struct.pack("I4x", V4L2_BUF_TYPE_VIDEO_OUTPUT) + pix)
        buf += bytes(_V4L2_FORMAT_SIZE - len(buf))
        try:
            fcntl.ioctl(self.fd, VIDIOC_S_FMT, buf)
        except OSError:
            os.close(self.fd)
            raise

    def write(self, data) -> None:
        mv = memoryview(data).cast("B")
        if len(mv) != self.frame_size:
            raise ValueError(f"frame is {len(mv)} bytes, expected {self.frame_size}")
        os.write(self.fd, mv)

    def close(self) -> None:
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1


def find_loopback_device() -> str | None:
    """Return the first /dev/videoN that belongs to v4l2loopback."""
    base = "/sys/devices/virtual/video4linux"
    try:
        names = sorted(os.listdir(base), key=lambda n: int(n.removeprefix("video") or 0))
    except OSError:
        return None
    for name in names:
        if name.startswith("video"):
            return f"/dev/{name}"
    return None
