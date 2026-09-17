"""Bounded USB framing for the Waveshare satellite. No board resets on open."""
import json
import struct
import time
import queue
import threading
import asyncio
import contextlib
import re

from websockets.asyncio.client import connect

MAGIC = b"HV"
MAX_PAYLOAD = 640


def encode_packet(kind, payload):
    if kind not in (1, 2, 3) or len(payload) > MAX_PAYLOAD:
        raise ValueError("Invalid USB packet")
    return MAGIC + struct.pack("<BH", kind, len(payload)) + payload


class CrashCapture:
    """Retain only recognized panic categories and code addresses, never raw logs."""
    def __init__(self):
        self.pending = bytearray()
        self.lines = []

    def feed(self, data):
        self.pending.extend(data)
        while b'\n' in self.pending:
            raw, _, rest = self.pending.partition(b'\n')
            self.pending = bytearray(rest)
            line = raw.decode('ascii', errors='replace').strip()
            safe = None
            if line.startswith('Backtrace:'):
                pairs = re.findall(r'0x[0-9a-fA-F]{8}:0x[0-9a-fA-F]{8}', line)
                if pairs: safe = 'Backtrace: ' + ' '.join(pairs[:32])
            elif line.startswith('Guru Meditation Error:'):
                match = re.search(r'\b(LoadProhibited|StoreProhibited|InstrFetchProhibited|IllegalInstruction|LoadStoreAlignment|IntegerDivideByZero|Interrupt wdt timeout|Unhandled debug exception)\b', line)
                if match: safe = 'Panic: ' + match.group(1)
            elif line.startswith('abort() was called at PC '):
                match = re.search(r'PC (0x[0-9a-fA-F]{8})', line)
                if match: safe = 'Abort at ' + match.group(1)
            if safe:
                self.lines.append(safe)
                self.lines[:] = self.lines[-12:]
        self.pending[:] = self.pending[-4096:]


class Decoder:
    def __init__(self, discarded=None):
        self.buffer = bytearray()
        self.discarded = discarded

    def feed(self, data):
        self.buffer.extend(data)
        packets = []
        while len(self.buffer) >= 5:
            start = self.buffer.find(MAGIC)
            if start < 0:
                if self.discarded: self.discarded(bytes(self.buffer[:-1]))
                self.buffer[:] = self.buffer[-1:]
                break
            if start:
                if self.discarded: self.discarded(bytes(self.buffer[:start]))
                del self.buffer[:start]
            if len(self.buffer) < 5:
                break
            kind, length = struct.unpack_from("<BH", self.buffer, 2)
            if kind not in (1, 2, 3) or length > MAX_PAYLOAD:
                del self.buffer[:2]
                continue
            if len(self.buffer) < 5 + length:
                break
            packets.append((kind, bytes(self.buffer[5:5+length])))
            del self.buffer[:5+length]
        return packets


def open_board(port):
    import serial
    device = serial.Serial(port=None, baudrate=115200, timeout=0.05, write_timeout=1)
    device.dtr = False
    device.rts = False
    device.port = port
    device.open()
    device.reset_input_buffer()
    return device


def diagnose(port, seconds=8, tone=False):
    decoder = Decoder()
    with open_board(port) as device:
        device.write(encode_packet(1, b"I"))
        deadline = time.monotonic() + seconds
        identified = False
        next_identify = time.monotonic() + 0.5
        while time.monotonic() < deadline:
            if not identified and time.monotonic() >= next_identify:
                device.write(encode_packet(1, b"I"))
                next_identify = time.monotonic() + 0.5
            for kind, payload in decoder.feed(device.read(2048)):
                if kind == 3:
                    try:
                        info = json.loads(payload)
                    except (ValueError, UnicodeDecodeError):
                        continue
                    if info.get("type") == "device":
                        if tone and not identified:
                            device.write(encode_packet(1, b"T"))
                        identified = True
                        print("Firmware:", info.get("firmware"), "AEC:", info.get("aec"))
                    elif info.get("type") == "levels" and identified:
                        print("Microphone peaks:", info.get("mic1"), info.get("mic2"),
                              "Playback reference:", info.get("reference"))
        device.write(encode_packet(1, b"X"))
        if not identified:
            raise RuntimeError("Home Voice firmware did not identify itself on this port")


class BoardLink:
    def __init__(self, port):
        self.device = open_board(port)
        self.lock = threading.Lock()
        self.audio = queue.Queue(maxsize=25)
        self.stop = threading.Event()
        self.active = threading.Event()
        self.closed = threading.Event()
        self.info = {}
        self.levels = {}
        self.setup = {}
        self.volume = None
        self.volume_ack = threading.Event()
        self.volume_lock = threading.Lock()
        self.native = {}
        self.failure = {}
        self.playback = {}
        self.upload = {}
        self.microphone = {}
        self.crash = CrashCapture()
        self.wake = {}
        self.wake_ack = threading.Event()
        self.wake_lock = threading.Lock()
        self.wake_ok = False
        self.native_ack = threading.Event()
        self.key_ack = threading.Event()
        self.key_lock = threading.Lock()
        self.key_save_ok = False
        self.error = ""
        self.result = {}
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.command(b"I")

    def send(self, kind, data):
        with self.lock:
            self.device.write(encode_packet(kind, data))

    def command(self, data):
        self.send(1, data)

    def set_volume(self, value):
        if type(value) is not int or not 0 <= value <= 80:
            raise ValueError("Speaker volume must be an integer from 0 to 80.")
        with self.volume_lock:
            self.volume_ack.clear()
            self.command(bytes((ord('V'), value)))
            if not self.volume_ack.wait(2) or self.volume != value:
                raise RuntimeError("The board did not confirm the volume change.")

    def _read(self):
        decoder = Decoder(self.crash.feed)
        heartbeat = time.monotonic()
        try:
            while not self.closed.is_set():
                for kind, data in decoder.feed(self.device.read(2048)):
                    if kind == 2 and self.active.is_set() and not self.stop.is_set():
                        try:
                            self.audio.put_nowait(data)
                        except queue.Full:
                            self.error = "Microphone backlog; conversation stopped."
                            self.stop.set()
                    elif kind == 3:
                        event = json.loads(data)
                        category = event.get("type")
                        if category == "device": self.info = event
                        elif category == "levels": self.levels = event
                        elif category == "setup": self.setup = event
                        elif category == "audio_settings":
                            self.volume = event.get("volume")
                            self.volume_ack.set()
                        elif category == "settings_error": self.volume_ack.set()
                        elif category == "device_state":
                            self.native = event
                            self.native_ack.set()
                        elif category == "wake_status": self.wake = event
                        elif category == "voice_failure": self.failure = event
                        elif category == "playback_status": self.playback = event
                        elif category == "upload_status": self.upload = event
                        elif category == "microphone_status": self.microphone = event
                        elif category in ("wake_saved", "wake_error"):
                            self.wake_ok = category == "wake_saved"
                            self.wake_ack.set()
                        elif category in ("key_saved", "key_error"):
                            self.key_save_ok = category == "key_saved"
                            self.key_ack.set()
                        elif category == "error":
                            self.error = "Board reported an audio error."
                            self.stop.set()
                if time.monotonic() - heartbeat > 1:
                    self.command(b"N" if self.info.get("standalone") else (b"P" if self.info else b"I"))
                    if self.info.get("standalone"):
                        self.command(b"Q")
                        self.command(b"F")
                        self.command(b"B")
                        self.command(b"U")
                        self.command(b"M")
                    heartbeat = time.monotonic()
        except Exception:
            if not self.closed.is_set():
                self.error = "USB connection lost. Reconnect the board and restart PC setup."
                self.stop.set()

    def configure_wake(self, data):
        if 'testing' in data:
            if type(data['testing']) is not bool: raise ValueError('Invalid test setting')
            command=bytes((ord('W'),2 if data['testing'] else 3))
        else:
            enabled,phrase,sensitivity=(data.get(k) for k in ('enabled','phrase','sensitivity'))
            if type(enabled) is not bool or type(phrase) is not int or phrase not in (0,1) or type(sensitivity) is not int or not 0<=sensitivity<=100:
                raise ValueError('Invalid wake-word settings')
            command=bytes((ord('W'),enabled,phrase,sensitivity))
        with self.wake_lock:
            self.wake_ack.clear();self.wake_ok=False
            self.command(command)
            if not self.wake_ack.wait(5) or not self.wake_ok:raise RuntimeError('The board did not confirm wake-word settings.')
            self.command(b'Q')

    def save_key(self, key):
        if not isinstance(key,str) or not 20 <= len(key) <= 512 or not key.isascii() or any(ord(c)<33 or ord(c)>126 for c in key):
            raise ValueError("Invalid key format")
        if not self.native.get("secure_storage"):
            raise ValueError("Encrypted device storage needs its one-time hardware setup before saving a key.")
        with self.key_lock:
            self.key_ack.clear()
            self.key_save_ok = False
            self.command(b"K"+key.encode("ascii"))
            if not self.key_ack.wait(5) or not self.key_save_ok:
                raise RuntimeError("The board did not confirm saving the key. Please try again.")

    async def converse(self, url, token=""):
        if not self.info:
            raise RuntimeError("The board has not identified itself yet")
        self.stop.clear()
        self.error = ""
        self.result = {}
        while not self.audio.empty():
            self.audio.get_nowait()
        headers = {"Authorization": f"Bearer {token}"} if token else None
        try:
            async with connect(url, additional_headers=headers, compression=None, max_queue=4,
                               max_size=8192, close_timeout=3) as socket:
                await socket.send(json.dumps({"type":"start", "version":1, "encoding":"pcm_s16le",
                                              "sample_rate":16000, "channels":1}))
                event = json.loads(await asyncio.wait_for(socket.recv(), 25))
                if event.get("type") != "ready":
                    raise RuntimeError("Voice service could not start. Check the API key and model access.")
                self.active.set()
                self.command(b"S")

                async def send_microphone():
                    while not self.stop.is_set():
                        try:
                            audio = self.audio.get_nowait()
                        except queue.Empty:
                            await asyncio.sleep(0.005)
                            continue
                        await socket.send(audio)
                    self.command(b"X")
                    await socket.send('{"type":"stop"}')

                sender = asyncio.create_task(send_microphone())
                async def receive():
                    async for message in socket:
                        if isinstance(message, bytes):
                            if not self.stop.is_set():
                                self.send(2, message)
                            continue
                        event = json.loads(message)
                        if event.get("type") == "closed":
                            self.result = event
                            return
                        if event.get("type") == "error":
                            self.error = "Voice service reported an error. Check API access and billing."
                            self.stop.set()
                receiver = asyncio.create_task(receive())
                try:
                    done, _ = await asyncio.wait([sender, receiver], return_when=asyncio.FIRST_COMPLETED)
                    for task in done: task.result()
                    if not receiver.done():
                        await asyncio.wait_for(receiver, 18)
                finally:
                    sender.cancel(); receiver.cancel()
                    await asyncio.gather(sender, receiver, return_exceptions=True)
        finally:
            self.active.clear()
            with contextlib.suppress(Exception): self.command(b"X")

    def close(self):
        self.stop.set()
        with contextlib.suppress(Exception): self.command(b"X")
        self.closed.set()
        self.reader.join(timeout=2)
        self.device.close()
