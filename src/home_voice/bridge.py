"""One audio device per voice session; the upstream credentials stay here."""

import asyncio
import base64
import contextlib
import hmac
import json
import logging
import math
import struct
import time
from http import HTTPStatus

from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed

from .config import FORMAT, FRAME_BYTES, LIVE_URL, Settings

log = logging.getLogger(__name__)


async def send_json(socket, payload):
    await asyncio.wait_for(socket.send(json.dumps(payload)), timeout=2)


class MockLive:
    """Offline protocol fixture, not a simulated language model or mic loopback."""
    def __init__(self):
        self.events = asyncio.Queue()
        self.started_at = time.monotonic()

    async def send(self, raw):
        event = json.loads(raw)
        if event["type"] == "session.start":
            await self.events.put({"type": "session.started", "session": {"id": "local-test"}})
            tone = b"".join(struct.pack("<h", int(1800 * math.sin(2 * math.pi * 440 * i / 16000)))
                            for i in range(1600))
            await self.events.put({"type": "session.output_audio.delta",
                                   "delta": base64.b64encode(tone).decode()})
        elif event["type"] == "session.close":
            await self.events.put({"type": "session.closed", "usage": {
                "seconds": round(time.monotonic() - self.started_at, 2)}})

    async def recv(self):
        return json.dumps(await self.events.get())


@contextlib.asynccontextmanager
async def live_connection(settings):
    if settings.mock:
        yield MockLive()
    else:
        async with connect(LIVE_URL, additional_headers={"Authorization": f"Bearer {settings.api_key}"},
                           compression=None, open_timeout=15, close_timeout=3,
                           max_size=2**20, max_queue=8, write_limit=8192) as socket:
            yield socket


class Bridge:
    def __init__(self, settings: Settings, connector=live_connection):
        self.settings = settings
        self.connector = connector
        self.busy = False

    def authorize(self, connection, request):
        if request.path != "/audio":
            return connection.respond(HTTPStatus.NOT_FOUND, "Use /audio\n")
        # Native clients only; reject browser pages connecting to a local service.
        if request.headers.get_all("Origin"):
            return connection.respond(HTTPStatus.FORBIDDEN, "Browser origins are not accepted\n")
        if self.settings.device_token:
            headers = request.headers.get_all("Authorization")
            if len(headers) != 1 or not hmac.compare_digest(
                    headers[0], f"Bearer {self.settings.device_token}"):
                return connection.respond(HTTPStatus.UNAUTHORIZED, "Device token required\n")

    async def handler(self, device):
        if self.busy:
            await send_json(device, {"type": "error", "message": "A conversation is already active"})
            return
        self.busy = True
        try:
            start = json.loads(await asyncio.wait_for(device.recv(), 10))
            if not isinstance(start, dict) or start != {"type": "start", "version": 1, **FORMAT}:
                raise ValueError("Expected protocol v1 start with mono PCM16 at 16000 Hz")
            if not self.settings.mock and not self.settings.api_key:
                raise ValueError("Set OPENAI_API_KEY on the host or run serve --ask-key")
            async with self.connector(self.settings) as upstream:
                await self.relay(device, upstream)
        except ConnectionClosed:
            pass
        except (ValueError, TypeError, TimeoutError) as exc:
            with contextlib.suppress(ConnectionClosed, TimeoutError):
                # Validation messages are authored here, not raw provider payloads.
                message = str(exc) if isinstance(exc, ValueError) and not isinstance(exc, json.JSONDecodeError) else "Invalid message or connection timeout"
                await send_json(device, {"type": "error", "message": message})
        except Exception as exc:
            log.error("Session failed (%s)", type(exc).__name__)
            with contextlib.suppress(ConnectionClosed, TimeoutError):
                await send_json(device, {"type": "error", "message":
                    "Voice connection failed. Check API access, billing, and network connectivity."})
        finally:
            self.busy = False

    async def relay(self, device, upstream):
        started = asyncio.Event()
        finalized = asyncio.Event()
        stopping = asyncio.Event()
        last_activity = time.monotonic()
        input_bytes = 0
        began = time.monotonic()

        async def receive_live():
            nonlocal last_activity
            while True:
                event = json.loads(await upstream.recv())
                kind = event.get("type")
                if kind == "session.started":
                    started.set()
                    await send_json(device, {"type": "ready", "version": 1, **FORMAT,
                                             "mock": self.settings.mock})
                elif kind == "session.output_audio.delta" and not stopping.is_set():
                    audio = base64.b64decode(event["delta"], validate=True)
                    if len(audio) % 2:
                        raise ValueError("Provider sent an incomplete PCM sample")
                    # Output can include continuous silence; use transcripts for idle detection.
                    for offset in range(0, len(audio), FRAME_BYTES):
                        await asyncio.wait_for(device.send(audio[offset:offset + FRAME_BYTES]), 2)
                elif kind in ("session.input_transcript.delta", "session.output_transcript.delta"):
                    if event.get("delta", "").strip():
                        last_activity = time.monotonic()
                    await send_json(device, {"type": "transcript", "speaker":
                        "user" if "input" in kind else "assistant", "delta": event.get("delta", "")})
                elif kind == "session.closed":
                    finalized.set()
                    with contextlib.suppress(ConnectionClosed, TimeoutError):
                        await send_json(device, {"type": "closed", "finalized": True,
                                                "usage": event.get("usage", {}), "input_bytes": input_bytes})
                    return
                elif kind == "error":
                    raise RuntimeError("Provider rejected a command or the session")
                elif kind == "response.event":
                    nested = event.get("event", {})
                    if nested.get("type") in ("response.failed", "error"):
                        await send_json(device, {"type": "notice", "message":
                            "The reasoning backend failed. Check backend model access."})

        async def receive_device():
            nonlocal input_bytes
            async for message in device:
                if isinstance(message, bytes):
                    if not started.is_set():
                        raise ValueError("Wait for ready before sending audio")
                    if not message or len(message) % 2 or len(message) > 3200:
                        raise ValueError("Audio frames must contain 1-1600 complete PCM16 samples")
                    input_bytes += len(message)
                    await send_json(upstream, {"type": "session.input_audio.append",
                                              "audio": base64.b64encode(message).decode("ascii")})
                else:
                    command = json.loads(message)
                    if command == {"type": "stop"}:
                        return
                    raise ValueError("Unsupported command; expected stop or binary audio")

        async def limits():
            while True:
                await asyncio.sleep(0.1)
                now = time.monotonic()
                if not started.is_set() and now - began >= self.settings.startup_timeout:
                    raise TimeoutError("Voice startup timeout")
                if now - began >= self.settings.max_seconds:
                    await send_json(device, {"type": "notice", "message": "Conversation time limit reached"})
                    return
                if started.is_set() and now - last_activity >= self.settings.idle_seconds:
                    await send_json(device, {"type": "notice", "message": "Conversation ended after inactivity"})
                    return

        async def drain_final():
            # A broken device or provider error can finish the regular receiver early.
            # Continue reading the upstream terminal event after requesting closure.
            while True:
                event = json.loads(await upstream.recv())
                if event.get("type") == "session.closed":
                    finalized.set()
                    with contextlib.suppress(ConnectionClosed, TimeoutError):
                        await send_json(device, {"type": "closed", "finalized": True,
                                                "usage": event.get("usage", {}), "input_bytes": input_bytes})
                    return

        receiver = asyncio.create_task(receive_live())
        tasks = [asyncio.create_task(receive_device()), asyncio.create_task(limits())]
        try:
            await send_json(upstream, {"type": "session.start", "session": self.settings.session()})
            done, _ = await asyncio.wait([receiver, *tasks], return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        except Exception as exc:
            message = str(exc) if isinstance(exc, ValueError) and not isinstance(exc, json.JSONDecodeError) else (
                "Voice session failed. Check API access, billing, selected backend model, and network connectivity.")
            log.warning("Voice session ended (%s)", type(exc).__name__)
            with contextlib.suppress(ConnectionClosed, TimeoutError):
                await send_json(device, {"type": "error", "message": message})
        finally:
            stopping.set()
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            if not finalized.is_set():
                try:
                    if receiver.done():
                        await asyncio.gather(receiver, return_exceptions=True)
                        receiver = asyncio.create_task(drain_final())
                    await send_json(upstream, {"type": "session.close"})
                    await asyncio.wait_for(asyncio.shield(receiver), self.settings.close_timeout)
                except (Exception, asyncio.CancelledError):
                    log.warning("Final session usage was not confirmed")
                    with contextlib.suppress(ConnectionClosed, TimeoutError):
                        await send_json(device, {"type": "closed", "finalized": False})
            receiver.cancel()
            await asyncio.gather(receiver, return_exceptions=True)


async def serve_bridge(settings, ssl_context=None):
    if settings.host not in ("127.0.0.1", "localhost", "::1"):
        if len(settings.device_token) < 32 or ssl_context is None:
            raise ValueError("Network listening requires TLS and a VOICE_DEVICE_TOKEN of at least 32 characters")
    if settings.max_seconds <= 0 or settings.idle_seconds <= 0:
        raise ValueError("Session limits must be positive")
    bridge = Bridge(settings)
    async with serve(bridge.handler, settings.host, settings.port,
                     process_request=bridge.authorize, ssl=ssl_context,
                     max_size=4096, max_queue=4, write_limit=4096,
                     compression=None, close_timeout=3):
        mode = "LOCAL TEST (no API calls)" if settings.mock else "GPT-Live"
        print(f"{mode} service ready on port {settings.port}. Start the audio client to talk.", flush=True)
        await asyncio.Future()
