"""Small, bounded PCM buffers shared by audio callbacks and the event loop."""

from array import array
from collections import deque
from threading import Lock
import sys


def select_channel(data: bytes, channels: int, channel: int) -> bytes:
    if channels < 1 or not 0 <= channel < channels or len(data) % (2 * channels):
        raise ValueError("Invalid PCM channel selection or incomplete audio frame")
    if channels == 1:
        return data
    samples = array("h")
    samples.frombytes(data)
    return samples[channel::channels].tobytes()


def duplicate_mono(data: bytes, channels: int) -> bytes:
    if channels < 1 or len(data) % 2:
        raise ValueError("Invalid output channel count or incomplete PCM sample")
    if channels == 1:
        return data
    samples = array("h")
    samples.frombytes(data)
    return array("h", (v for v in samples for _ in range(channels))).tobytes()


class PlaybackBuffer:
    def __init__(self, max_bytes=16000):  # 500 ms of mono PCM16 at 16 kHz
        self.max_bytes = max_bytes
        self._chunks = deque()
        self._size = 0
        self._lock = Lock()

    def append(self, data):
        if len(data) % 2:
            raise ValueError("Incomplete PCM sample")
        with self._lock:
            if self._size + len(data) > self.max_bytes:
                raise BufferError("Playback is more than 500 ms behind; ending the session")
            if data:
                self._chunks.append(data)
                self._size += len(data)

    def read(self, size):
        with self._lock:
            result = bytearray()
            while self._chunks and len(result) < size:
                chunk = self._chunks.popleft()
                take = min(len(chunk), size - len(result))
                result.extend(chunk[:take])
                self._size -= take
                if take < len(chunk):
                    self._chunks.appendleft(chunk[take:])
            return bytes(result).ljust(size, b"\0")

    def clear(self):
        with self._lock:
            self._chunks.clear()
            self._size = 0


def require_little_endian():
    if sys.byteorder != "little":
        raise RuntimeError("This audio client requires a little-endian machine")
