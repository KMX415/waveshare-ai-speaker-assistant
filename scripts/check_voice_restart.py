"""Exercise three short start/stop cycles with the saved device key."""
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
                for command in (b"N", b"D", b"Q"):
                    send(command)
                next_poll = time.monotonic() + .5
            for kind, data in decoder.feed(board.read(2048)):
                if kind == 3:
                    try:
                        event = json.loads(data)
                    except (ValueError, UnicodeDecodeError):
                        continue
                    reports[event.get("type")] = event

    poll(2)
    assert reports["device_state"]["key_saved"]
    assert not reports["device_state"]["active"]
    assert not reports["wake_status"]["testing"]
    for cycle in range(1, 4):
        try:
            send(b"G")
            deadline = time.monotonic() + 40
            while time.monotonic() < deadline:
                poll(1)
                state = reports["device_state"]
                if state["active"] and state["message"] == "Listening":
                    break
                if not state["active"]:
                    raise RuntimeError(f"Cycle {cycle} failed: {state['message']}; {reports.get('voice_diagnostics')}")
            else:
                raise RuntimeError(f"Cycle {cycle} never reached Listening")
            poll(5)
            assert reports["device_state"]["active"] and reports["device_state"]["message"] == "Listening", reports["device_state"]["message"]
            print(f"Cycle {cycle}: listening;", reports.get("voice_diagnostics"), flush=True)
        finally:
            send(b"Z")
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                poll(1)
                if not reports["device_state"]["active"]:
                    break
            print("Closed:", {k: reports["device_state"].get(k) for k in ("message", "finalized", "seconds")}, flush=True)
        assert reports["device_state"]["finalized"]
        poll(4)
    print("PASS: three consecutive voice connections and finalized shutdowns", flush=True)
