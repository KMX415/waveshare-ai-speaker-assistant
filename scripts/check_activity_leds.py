"""Verify LED DMA refresh and speaker-level input without starting a paid session."""
import json
import time
from home_voice.usb import Decoder, encode_packet, open_board

decoder = Decoder()
reports = {}

with open_board("COM14") as board:
    def read_for(seconds):
        deadline = time.monotonic() + seconds
        next_poll = 0
        while time.monotonic() < deadline:
            if time.monotonic() >= next_poll:
                for command in (b"I", b"Q", b"L", b"D"):
                    board.write(encode_packet(1, command))
                next_poll = time.monotonic() + 1
            for kind, payload in decoder.feed(board.read(2048)):
                if kind == 3:
                    try:
                        report = json.loads(payload)
                    except (ValueError, UnicodeDecodeError):
                        continue
                    reports[report.get("type")] = report

    read_for(8)
    assert reports["device"]["firmware"].endswith("0.6.1"), reports.get("device")
    assert reports["device_state"]["key_saved"]
    assert not reports["device_state"]["active"], "Conversation in progress; skip tone"
    before = dict(reports["led_status"])
    gaps = reports["wake_status"]["dropped"]
    assert before["frames"] > 0, "LED driver did not start"
    board.write(encode_packet(1, b"T"))
    read_for(4)
    after = reports["led_status"]
    assert after["frames"] > before["frames"] + 50, (before, after)
    assert after["audio_frames"] > before["audio_frames"], "Speaker PCM did not reach LED meter"
    assert reports["wake_status"]["dropped"] == gaps, "Wake microphone queue dropped samples"
    print("PASS: LED refresh, speaker-reactive meter, retained key, no new wake queue gaps")
    print("LED counters:", after)
    print("Wake:", reports["wake_status"])
    print("Audio:", reports["audio_settings"])
