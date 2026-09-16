"""Native desktop audio client; uses the same wire format planned for ESP32."""

import asyncio
import contextlib
import json
import queue
import signal
import time
from array import array
from collections import deque

import sounddevice as sd
from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosed

from .audio import PlaybackBuffer, duplicate_mono, require_little_endian, select_channel
from .bridge import send_json
from .config import FORMAT, FRAME_SAMPLES, SAMPLE_RATE


def devices():
    print(sd.query_devices())
    print("\nUse the ReSpeaker input and its speaker output, preferably on the same host API.")
    print("Device indices can change after unplugging USB devices. Check again after reconnecting.")


def check_audio(args):
    require_little_endian()
    if args.input_channels < 1 or not 0 <= args.channel < args.input_channels:
        raise ValueError("--channel must be between 0 and --input-channels minus one")
    sd.check_input_settings(device=args.input, channels=args.input_channels,
                            dtype="int16", samplerate=SAMPLE_RATE)
    if hasattr(args, "output"):
        sd.check_output_settings(device=args.output, channels=args.output_channels,
                                 dtype="int16", samplerate=SAMPLE_RATE)


def microphone_level(args):
    """Local measurement only; no files, network connection, or audio playback."""
    check_audio(args)
    print("Local microphone check. Speak normally; audio is not saved or sent anywhere.")
    with sd.RawInputStream(device=args.input, samplerate=SAMPLE_RATE,
                           channels=args.input_channels, dtype="int16",
                           blocksize=FRAME_SAMPLES) as stream:
        end = time.monotonic() + args.seconds
        count, peak, total, samples_count = 0, 0, 0, 0
        while time.monotonic() < end:
            raw, overflow = stream.read(FRAME_SAMPLES)
            if overflow:
                print("Input overflow: select another audio host API/device.")
            samples = array("h")
            samples.frombytes(select_channel(bytes(raw), args.input_channels, args.channel))
            peak = max(peak, max(map(abs, samples), default=0))
            total += sum(x * x for x in samples)
            samples_count += len(samples)
            count += 1
            if count % 25 == 0:
                rms = (total / max(samples_count, 1)) ** 0.5 / 32768
                print(f"Peak {peak / 32768:5.1%}  RMS {rms:5.1%}", flush=True)
                peak, total, samples_count = 0, 0, 0


async def talk(args, token="", ssl_context=None):
    check_audio(args)  # Fail before opening a billable session.
    playback = PlaybackBuffer()
    capture = queue.Queue(maxsize=10)
    faults = deque(maxlen=1)
    stopped = asyncio.Event()
    loop = asyncio.get_running_loop()
    old_handler = signal.signal(signal.SIGINT, lambda *_: loop.call_soon_threadsafe(stopped.set))

    def input_callback(data, frames, timing, status):
        if status:
            faults.append("Audio input overflow or device error")
        try:
            pcm = select_channel(bytes(data), args.input_channels, args.channel)
            capture.put_nowait(pcm)
        except (queue.Full, ValueError):
            faults.append("Microphone audio is falling behind; session stopped")

    def output_callback(data, frames, timing, status):
        if status:
            faults.append("Audio output underflow or device error")
        data[:] = duplicate_mono(playback.read(frames * 2), args.output_channels)

    headers = {"Authorization": f"Bearer {token}"} if token else {}
    finalized = False
    receiver = sender = waiter = None
    try:
        # Open audio devices before starting a paid session, but do not start capture yet.
        with contextlib.ExitStack() as audio_devices:
            input_stream = sd.RawInputStream(device=args.input, samplerate=SAMPLE_RATE, dtype="int16",
                                             channels=args.input_channels, blocksize=FRAME_SAMPLES,
                                             callback=input_callback)
            audio_devices.callback(input_stream.close)
            output_stream = sd.RawOutputStream(device=args.output, samplerate=SAMPLE_RATE, dtype="int16",
                                               channels=args.output_channels, blocksize=FRAME_SAMPLES,
                                               callback=output_callback)
            audio_devices.callback(output_stream.close)
            connection_args = dict(additional_headers=headers, compression=None,
                                   max_size=65536, max_queue=4, write_limit=4096,
                                   open_timeout=10, close_timeout=3)
            if ssl_context is not None:
                connection_args["ssl"] = ssl_context
            async with connect(args.url, **connection_args) as socket:
                await send_json(socket, {"type": "start", "version": 1, **FORMAT})
                first = json.loads(await asyncio.wait_for(socket.recv(), 25))
                if first.get("type") != "ready":
                    raise RuntimeError(first.get("message", "Service did not become ready"))
                if any(first.get(k) != v for k, v in FORMAT.items()) or first.get("version") != 1:
                    raise RuntimeError("Service negotiated an unsupported audio format")
                print("LOCAL TEST: a brief tone should play; there is no AI response." if first.get("mock")
                      else "Listening. Speak naturally; Ctrl+C ends the conversation.", flush=True)

                async def receive():
                    nonlocal finalized
                    async for message in socket:
                        if isinstance(message, bytes):
                            if not stopped.is_set():
                                playback.append(message)
                        else:
                            event = json.loads(message)
                            kind = event.get("type")
                            if kind == "error":
                                raise RuntimeError(event.get("message", "Voice service error"))
                            if kind == "notice":
                                print(event["message"], flush=True)
                            elif kind == "transcript" and args.transcripts:
                                print(f"{event['speaker']}: {event['delta']}", flush=True)
                            elif kind == "closed":
                                finalized = event.get("finalized", False)
                                if finalized:
                                    print(f"Session ended. Voice seconds: {event.get('usage', {}).get('seconds', 'unknown')}; "
                                          f"microphone bytes received: {event.get('input_bytes', 'unknown')}", flush=True)
                                else:
                                    raise RuntimeError("Session ended without confirmed final API usage")
                                return
                    raise RuntimeError("Connection lost before session finalization")

                async def send_audio():
                    while not stopped.is_set():
                        if faults:
                            raise RuntimeError(faults[-1])
                        if not input_stream.active or not output_stream.active:
                            raise RuntimeError("Audio device stopped or was disconnected")
                        try:
                            pcm = capture.get_nowait()
                        except queue.Empty:
                            await asyncio.sleep(0.005)
                            continue
                        await asyncio.wait_for(socket.send(pcm), 1)

                output_stream.start()
                input_stream.start()
                receiver = asyncio.create_task(receive())
                sender = asyncio.create_task(send_audio())
                waiter = asyncio.create_task(stopped.wait())
                try:
                    done, _ = await asyncio.wait([receiver, sender, waiter], return_when=asyncio.FIRST_COMPLETED)
                    for task in done:
                        task.result()
                finally:
                    stopped.set()
                    input_stream.abort()
                    playback.clear()
                    output_stream.abort()
                    sender.cancel()
                    waiter.cancel()
                    await asyncio.gather(sender, waiter, return_exceptions=True)
                    if not finalized:
                        with contextlib.suppress(ConnectionClosed, TimeoutError):
                            await send_json(socket, {"type": "stop"})
                        if not receiver.done():
                            try:
                                await asyncio.wait_for(asyncio.shield(receiver), 20)
                            except TimeoutError:
                                raise RuntimeError("Final session usage was not confirmed") from None
                    receiver.cancel()
                    await asyncio.gather(receiver, return_exceptions=True)
    finally:
        signal.signal(signal.SIGINT, old_handler)
        for task in (receiver, sender, waiter):
            if task is not None:
                task.cancel()
        await asyncio.gather(*(t for t in (receiver, sender, waiter) if t is not None), return_exceptions=True)


async def probe(url, token=""):
    """Protocol test using paced silence, with no microphone access."""
    headers = {"Authorization": f"Bearer {token}"} if token else {}
    async with connect(url, additional_headers=headers, compression=None) as socket:
        await send_json(socket, {"type": "start", "version": 1, **FORMAT})
        ready = json.loads(await asyncio.wait_for(socket.recv(), 25))
        if ready.get("type") != "ready":
            raise RuntimeError(ready.get("message", "Service not ready"))
        audio_bytes = 0
        for _ in range(10):
            await socket.send(b"\0" * FRAME_SAMPLES * 2)
            await asyncio.sleep(0.02)
        await send_json(socket, {"type": "stop"})
        async with asyncio.timeout(20):
            async for message in socket:
                if isinstance(message, bytes):
                    audio_bytes += len(message)
                else:
                    event = json.loads(message)
                    if event.get("type") == "error":
                        raise RuntimeError(event.get("message", "Service error"))
                    if event.get("type") == "closed":
                        if not event.get("finalized"):
                            raise RuntimeError("Finalization unconfirmed")
                        if event.get("input_bytes") != 6400:
                            raise RuntimeError("Service did not receive the complete microphone stream")
                        print(f"Probe passed. Sent 6400 microphone bytes; received {audio_bytes} speaker bytes. "
                              f"Mode: {'local test' if ready.get('mock') else 'LIVE API (billable)' }.")
                        return
        raise RuntimeError("Service disconnected without a closed event")
