"""Run the firmware phrase matcher checks without opening an API session."""
import json
import time
from home_voice.usb import Decoder, encode_packet, open_board

decoder = Decoder()
with open_board("COM14") as board:
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        board.write(encode_packet(1, b"J"))
        until = time.monotonic() + .5
        while time.monotonic() < until:
            for kind, payload in decoder.feed(board.read(2048)):
                if kind != 3:
                    continue
                try:
                    event = json.loads(payload)
                except ValueError:
                    continue
                if event.get("type") == "hangup_status":
                    assert event["selftest"], "Firmware phrase matching checks failed"
                    print("PASS: split fragments, punctuation, casing, word boundaries, and stale fragment expiry")
                    print("Voice hangups since boot:", event["hangups"])
                    raise SystemExit(0)
    raise RuntimeError("Firmware did not answer hangup diagnostic")
