"""Exercise the desktop client over real local sockets with a fake audio driver."""

import asyncio
import contextlib
import io
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from websockets.asyncio.server import serve

from home_voice.bridge import Bridge
from home_voice.client import talk
from home_voice.config import Settings


class FakeStream:
    instances = []

    def __init__(self, *, callback, channels, **kwargs):
        self.callback = callback
        self.channels = channels
        self.active = False
        self.closed = False
        self.started = False
        self.input = len(self.instances) % 2 == 0
        self.played = bytearray()
        self.instances.append(self)

    def start(self):
        self.active = self.started = True
        self.tick()

    def tick(self):
        if not self.active:
            return
        data = b"\x01\x00" * 320 * self.channels if self.input else bytearray(640 * self.channels)
        self.callback(data, 320, None, False)
        if not self.input:
            self.played.extend(data)
        asyncio.get_running_loop().call_later(0.02, self.tick)

    def abort(self):
        self.active = False

    def close(self):
        self.abort()
        self.closed = True


class ClientTests(unittest.IsolatedAsyncioTestCase):
    async def run_client(self, settings):
        FakeStream.instances = []
        bridge = Bridge(settings)
        async with serve(bridge.handler, "127.0.0.1", 0, compression=None) as server:
            port = server.sockets[0].getsockname()[1]
            args = SimpleNamespace(input=0, output=1, input_channels=1, output_channels=2,
                                   channel=0, transcripts=False, url=f"ws://127.0.0.1:{port}/audio")
            with patch("home_voice.client.sd.RawInputStream", FakeStream), \
                 patch("home_voice.client.sd.RawOutputStream", FakeStream), \
                 patch("home_voice.client.sd.check_input_settings"), \
                 patch("home_voice.client.sd.check_output_settings"):
                await asyncio.wait_for(talk(args), 2)

    async def test_capture_playback_and_clean_device_shutdown(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            await self.run_client(Settings(mock=True, max_seconds=0.25, idle_seconds=5))
        self.assertTrue(all(s.started and s.closed and not s.active for s in FakeStream.instances))
        self.assertTrue(any(FakeStream.instances[1].played))
        self.assertIn("Session ended", output.getvalue())
        self.assertNotIn("microphone bytes received: 0", output.getvalue())

    async def test_no_microphone_capture_when_service_refuses_start(self):
        with self.assertRaisesRegex(RuntimeError, "OPENAI_API_KEY"):
            await self.run_client(Settings(mock=False, api_key=""))
        self.assertTrue(all(s.closed and not s.started for s in FakeStream.instances))
