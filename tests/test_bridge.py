import asyncio
import base64
import contextlib
import json
import unittest

from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from websockets.exceptions import InvalidStatus

from home_voice.bridge import Bridge, MockLive, send_json
from home_voice.config import FORMAT, Settings


class RecordedLive(MockLive):
    def __init__(self, mode="normal"):
        super().__init__()
        self.mode = mode
        self.sent = []
        self.closed = asyncio.Event()

    async def send(self, raw):
        event = json.loads(raw)
        self.sent.append(event)
        if event["type"] == "session.close":
            self.closed.set()
            if self.mode == "no-final":
                return
        if event["type"] == "session.start" and self.mode == "stall":
            return
        if event["type"] == "session.start" and self.mode == "error":
            await self.events.put({"type": "error", "error": {"message": "PRIVATE PROVIDER DETAIL"}})
            return
        await super().send(raw)


class BridgeTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.live = RecordedLive()
        self.settings = Settings(mock=True, max_seconds=5, idle_seconds=5,
                                 startup_timeout=0.2, close_timeout=0.15)

        @contextlib.asynccontextmanager
        async def connector(settings):
            yield self.live

        self.bridge = Bridge(self.settings, connector)
        self.server = await serve(self.bridge.handler, "127.0.0.1", 0,
                                  process_request=self.bridge.authorize, max_size=4096,
                                  compression=None, close_timeout=0.2)
        self.url = f"ws://127.0.0.1:{self.server.sockets[0].getsockname()[1]}/audio"

    async def asyncTearDown(self):
        self.server.close()
        await self.server.wait_closed()

    async def start(self, socket):
        await send_json(socket, {"type": "start", "version": 1, **FORMAT})
        result = json.loads(await asyncio.wait_for(socket.recv(), 1))
        self.assertEqual(result["type"], "ready")
        self.assertEqual(result["sample_rate"], 16000)

    async def collect(self, socket):
        events, audio = [], bytearray()
        async with asyncio.timeout(2):
            async for message in socket:
                if isinstance(message, bytes):
                    audio.extend(message)
                else:
                    event = json.loads(message)
                    events.append(event)
                    if event["type"] == "closed":
                        break
        return events, audio

    async def test_full_audio_roundtrip_and_confirmed_stop(self):
        async with connect(self.url) as socket:
            await self.start(socket)
            first_audio = await socket.recv()
            self.assertIsInstance(first_audio, bytes)
            raw = b"\x00\x01\x00\x02" * 160
            await socket.send(raw)
            await send_json(socket, {"type": "stop"})
            events, remaining_audio = await self.collect(socket)
            self.assertEqual(len(first_audio) + len(remaining_audio), 3200)
            self.assertTrue(events[-1]["finalized"])
            self.assertEqual(events[-1]["input_bytes"], len(raw))
            sent_audio = next(e for e in self.live.sent if e["type"] == "session.input_audio.append")
            self.assertEqual(base64.b64decode(sent_audio["audio"]), raw)
            session = self.live.sent[0]["session"]
            self.assertEqual(session["model"], "gpt-live-1")
            self.assertEqual(session["audio"]["format"]["rate"], 16000)
            self.assertTrue(self.live.closed.is_set())

    async def test_disconnect_requests_upstream_close(self):
        async with connect(self.url) as socket:
            await self.start(socket)
        await asyncio.wait_for(self.live.closed.wait(), 1)

    async def test_invalid_sample_rate_never_opens_upstream(self):
        async with connect(self.url) as socket:
            await send_json(socket, {"type": "start", "version": 1, **FORMAT, "sample_rate": 48000})
            error = json.loads(await socket.recv())
            self.assertEqual(error["type"], "error")
            self.assertFalse(self.live.sent)

    async def test_odd_pcm_rejected_and_session_finalized(self):
        async with connect(self.url) as socket:
            await self.start(socket)
            await socket.send(b"x")
            events, _ = await self.collect(socket)
            self.assertIn("error", [e["type"] for e in events])
            self.assertTrue(events[-1]["finalized"])

    async def test_second_client_cannot_start_a_second_paid_session(self):
        async with connect(self.url) as first:
            await self.start(first)
            async with connect(self.url) as second:
                event = json.loads(await second.recv())
                self.assertEqual(event["type"], "error")
                self.assertEqual(sum(e["type"] == "session.start" for e in self.live.sent), 1)
            await send_json(first, {"type": "stop"})
            await self.collect(first)

    async def test_idle_silence_ends_paid_session(self):
        self.settings.idle_seconds = 0.15
        async with connect(self.url) as socket:
            await self.start(socket)
            events, _ = await self.collect(socket)
            self.assertTrue(events[-1]["finalized"])
            self.assertTrue(any("inactivity" in e.get("message", "") for e in events))

    async def test_max_duration_ends_paid_session(self):
        self.settings.max_seconds = 0.15
        async with connect(self.url) as socket:
            await self.start(socket)
            events, _ = await self.collect(socket)
            self.assertTrue(events[-1]["finalized"])
            self.assertTrue(any("time limit" in e.get("message", "") for e in events))

    async def test_startup_timeout_closes_upstream(self):
        self.live.mode = "stall"
        async with connect(self.url) as socket:
            await send_json(socket, {"type": "start", "version": 1, **FORMAT})
            events, _ = await self.collect(socket)
            self.assertEqual(events[0]["type"], "error")
            self.assertTrue(self.live.closed.is_set())
            self.assertTrue(events[-1]["finalized"])

    async def test_audio_before_ready_rejected(self):
        self.live.mode = "stall"
        async with connect(self.url) as socket:
            await send_json(socket, {"type": "start", "version": 1, **FORMAT})
            await socket.send(b"\0\0")
            events, _ = await self.collect(socket)
            self.assertIn("Wait for ready", events[0]["message"])
            self.assertTrue(self.live.closed.is_set())

    async def test_missing_terminal_event_is_reported(self):
        self.live.mode = "no-final"
        async with connect(self.url) as socket:
            await self.start(socket)
            await send_json(socket, {"type": "stop"})
            events, _ = await self.collect(socket)
            self.assertFalse(events[-1]["finalized"])

    async def test_provider_errors_are_not_leaked_and_close_is_attempted(self):
        self.live.mode = "error"
        async with connect(self.url) as socket:
            await send_json(socket, {"type": "start", "version": 1, **FORMAT})
            events, _ = await self.collect(socket)
            self.assertEqual(events[0]["type"], "error")
            self.assertNotIn("PRIVATE PROVIDER DETAIL", json.dumps(events))
            self.assertTrue(self.live.closed.is_set())

    async def test_auth_rejects_before_provider_connection(self):
        self.settings.device_token = "test-token" * 4
        with self.assertRaises(InvalidStatus) as caught:
            async with connect(self.url):
                pass
        self.assertEqual(caught.exception.response.status_code, 401)
        self.assertFalse(self.live.sent)
        async with connect(self.url, additional_headers={"Authorization": f"Bearer {self.settings.device_token}"}) as socket:
            await self.start(socket)
            await send_json(socket, {"type": "stop"})
            await self.collect(socket)

    async def test_browser_origin_rejected_before_provider_connection(self):
        with self.assertRaises(InvalidStatus) as caught:
            async with connect(self.url, origin="https://example.com"):
                pass
        self.assertEqual(caught.exception.response.status_code, 403)
        self.assertFalse(self.live.sent)

    async def test_missing_key_does_not_open_provider(self):
        self.settings.mock = False
        async with connect(self.url) as socket:
            await send_json(socket, {"type": "start", "version": 1, **FORMAT})
            event = json.loads(await socket.recv())
            self.assertIn("OPENAI_API_KEY", event["message"])
            self.assertFalse(self.live.sent)
