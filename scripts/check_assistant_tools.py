"""Paid on-device smoke test. Requires the setup service to release COM14.

Exercises actual hosted search and a same-volume function call; never reads keys,
Wi-Fi credentials, conversation transcripts or private location preferences.
"""
import json
import time
from home_voice.usb import Decoder, encode_packet, open_board

reports = {}
decoder = Decoder()
with open_board("COM14") as board:
    def send(payload):
        board.write(encode_packet(1, payload))

    def poll(seconds):
        end = time.monotonic() + seconds
        next_poll = 0
        while time.monotonic() < end:
            if time.monotonic() >= next_poll:
                for command in (b"N", b"U", b"B", b"F"):
                    send(command)
                next_poll = time.monotonic() + 1
            for kind, payload in decoder.feed(board.read(2048)):
                if kind != 3:
                    continue
                try:
                    event = json.loads(payload)
                except (ValueError, UnicodeDecodeError):
                    continue
                reports[event.get("type")] = event

    def wait_for(predicate, seconds=60):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            poll(1)
            state = reports.get("device_state", {})
            if predicate():
                return
            if state.get("failed") or (not state.get("active") and state.get("message", "").startswith("OpenAI")):
                raise RuntimeError(state.get("message"))
        raise RuntimeError("Timed out: " + json.dumps({k: reports.get(k) for k in ("upload_status", "voice_failure")}))

    send(b"I")
    poll(2)
    assert not reports["device_state"]["active"], "Do not interrupt an active conversation"
    send(b"A")
    poll(2)
    assert reports.get("assistant_test", {}).get("checks") == 255, reports.get("assistant_test")
    print("PASS: tool parsing, duplicate events, cancellation and invalid argument checks", flush=True)
    # Reading the current volume avoids changing the user's preferred setting.
    volume = reports.get("audio_settings", {}).get("volume")
    assert isinstance(volume, int) and 0 <= volume <= 80, reports.get("audio_settings")
    try:
        send(b"G")
        wait_for(lambda: reports.get("device_state", {}).get("message") == "Listening", 45)
        send(b"YUse live web search to look up today's weather in Boston, Massachusetts, USA. Give one short sentence and name the source.")
        wait_for(lambda: reports.get("upload_status", {}).get("searches", 0) >= 1, 75)
        print("PASS: real hosted weather search completed", flush=True)
        poll(15)
        send(b"Y" + f"Use set_volume to set your speaker volume to exactly {volume}. Then briefly confirm the tool result.".encode())
        wait_for(lambda: reports.get("upload_status", {}).get("functions", 0) >= 1, 60)
        poll(12)
        assert reports["device_state"]["active"], reports["device_state"]["message"]
        assert reports["upload_status"].get("backend_errors", 0) == 0
        print("PASS: follow-up device function completed; conversation remained active", flush=True)
        print("Transport/tool counters:", reports["upload_status"], flush=True)
    finally:
        send(b"Z")
        wait_for(lambda: not reports.get("device_state", {}).get("active"), 25)
        assert reports["device_state"].get("finalized"), "Final usage was not confirmed"
        print("PASS: clean finalized shutdown", flush=True)
