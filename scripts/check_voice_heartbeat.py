"""Paid hardware regression: survive the former ~130-second PONG timeout.

Run with the PC setup service stopped. Uses the device's saved key and prints
only counters, never credentials or transcripts. Leaves an active user session
untouched. Short typed requests prevent the normal 60-second idle timeout.
"""
import json
import time
from home_voice.usb import Decoder, encode_packet, open_board

reports = {}
decoder = Decoder()
with open_board("COM14") as board:
    def send(command):
        board.write(encode_packet(1, command))

    def poll(seconds):
        end = time.monotonic() + seconds
        next_poll = 0
        while time.monotonic() < end:
            if time.monotonic() >= next_poll:
                for command in (b"N", b"U", b"F"):
                    send(command)
                next_poll = time.monotonic() + .5
            for kind, payload in decoder.feed(board.read(2048)):
                if kind == 3:
                    try:
                        event = json.loads(payload)
                    except (ValueError, UnicodeDecodeError):
                        continue
                    reports[event.get("type")] = event

    poll(2)
    assert not reports["device_state"]["active"], "User conversation active"
    failures = reports.get("voice_failure", {}).get("count", 0)
    try:
        send(b"G")
        for _ in range(45):
            poll(1)
            if reports["device_state"]["message"] == "Listening":
                break
        else:
            raise RuntimeError("Session did not become ready")
        began = time.monotonic()
        for cycle in range(6):
            send(b"YSay just the word ready.")
            for _ in range(6):
                poll(5)
                assert reports["device_state"]["active"], reports["device_state"]["message"]
                assert reports["voice_failure"]["count"] == failures, reports["voice_failure"]
            upload = reports["upload_status"]
            assert upload.get("pongs", 0) >= cycle + 1, "Heartbeat replies missing"
            print(f"{time.monotonic()-began:.0f}s: active; pongs={upload['pongs']}; dropped_ms={upload['dropped_ms']}", flush=True)
        print("PASS: session survived 180 seconds with received heartbeats and no new failures", flush=True)
    finally:
        send(b"Z")
        for _ in range(25):
            poll(1)
            if not reports["device_state"]["active"]:
                break
        assert not reports["device_state"]["active"]
        assert reports["device_state"]["finalized"]
        print("PASS: finalized shutdown", flush=True)
