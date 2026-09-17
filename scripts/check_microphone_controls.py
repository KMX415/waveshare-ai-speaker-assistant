"""Paid board smoke test for mic controls; no calibration speech is fabricated.

Stop the PC setup service first. Runs the deterministic calibration decision
tests, switches automatic gain through model tools, then restores its old value.
"""
import json
import time
from home_voice.usb import Decoder, encode_packet, open_board

reports = {}
decoder = Decoder()
with open_board('COM14') as board:
    def send(command): board.write(encode_packet(1, command))

    def poll(seconds):
        end = time.monotonic() + seconds
        next_poll = 0
        while time.monotonic() < end:
            if time.monotonic() >= next_poll:
                for command in (b'N', b'M', b'U'):
                    send(command)
                next_poll = time.monotonic() + .5
            for kind, payload in decoder.feed(board.read(2048)):
                if kind != 3: continue
                try: event = json.loads(payload)
                except (ValueError, UnicodeDecodeError): continue
                if 'selftest' in event: reports['selftest'] = event['selftest']
                reports[event.get('type')] = event

    def wait(predicate, seconds=45):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            poll(.5)
            if predicate(): return
        raise RuntimeError('Microphone test timed out')

    poll(2)
    assert not reports['device_state']['active'], 'User conversation active'
    original = reports['microphone_status']['automatic']
    original_gain = reports['microphone_status']['gain_db']
    assert reports['microphone_status']['available'], 'Speech detector unavailable'
    send(b'M\x01');poll(2)
    assert reports.get('selftest') == 255, reports.get('selftest')
    print('PASS: speech/noise, clipping, short samples and gain-bound decision tests', flush=True)
    try:
        send(b'G')
        wait(lambda: reports['device_state']['message'] == 'Listening')
        enabled = 'false' if original else 'true'
        send(b'Y' + f'Call set_auto_gain with enabled {enabled}, then briefly confirm.'.encode())
        wait(lambda: reports['microphone_status']['automatic'] != original)
        poll(4)
        print('PASS: live tool changed automatic microphone gain', flush=True)
        enabled = 'true' if original else 'false'
        send(b'Y' + f'Call set_auto_gain with enabled {enabled}, then briefly confirm.'.encode())
        wait(lambda: reports['microphone_status']['automatic'] == original)
        poll(4)
        assert reports['microphone_status']['gain_db'] == original_gain
        assert reports['upload_status']['functions'] >= 2
        print('PASS: original automatic/hardware gain restored; upload remains active', flush=True)
    finally:
        send(b'Z')
        wait(lambda: not reports['device_state']['active'],25)
        assert reports['device_state']['finalized']
