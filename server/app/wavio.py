"""Minimal streaming WAV writer (mono, 16-bit PCM)."""

from __future__ import annotations

import struct
from pathlib import Path

_HEADER = struct.Struct("<4sI4s4sIHHIIHH4sI")


class WavWriter:
    """Append PCM frames and patch the RIFF header on close.

    Deliberately uses the stdlib instead of a codec dependency: the device sends
    raw little-endian s16 frames, which is already a valid WAV payload.
    """

    def __init__(self, path: Path, rate: int = 16000, channels: int = 1) -> None:
        self.path = Path(path)
        self.rate = rate
        self.channels = channels
        self.frames = 0
        self._file = self.path.open("wb")
        self._file.write(b"\x00" * _HEADER.size)

    def write(self, pcm: bytes) -> None:
        if not pcm:
            return
        self._file.write(pcm)
        self.frames += len(pcm) // (2 * self.channels)

    def close(self) -> None:
        if self._file.closed:
            return
        block_align = 2 * self.channels
        data_size = self.frames * block_align
        header = _HEADER.pack(
            b"RIFF",
            36 + data_size,
            b"WAVE",
            b"fmt ",
            16,
            1,  # PCM
            self.channels,
            self.rate,
            self.rate * block_align,
            block_align,
            16,
            b"data",
            data_size,
        )
        self._file.seek(0)
        self._file.write(header)
        self._file.close()

    @property
    def duration(self) -> float:
        return self.frames / self.rate if self.rate else 0.0

    def __enter__(self) -> "WavWriter":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()
