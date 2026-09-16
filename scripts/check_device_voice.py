"""Bounded, explicitly invoked hardware voice check; never reads credentials."""
import json
import time
from home_voice.usb import Decoder, encode_packet, open_board

with open_board('COM14') as device:
    decoder = Decoder()
    def events():
        for kind, payload in decoder.feed(device.read(2048)):
            if kind != 3:
                continue
            try:
                yield json.loads(payload)
            except ValueError:
                pass
    def send(command):
        device.write(encode_packet(1, command))

    send(b'N')
    deadline = time.monotonic() + 5
    state = {}
    while time.monotonic() < deadline and not state:
        for event in events():
            if event.get('type') == 'device_state':
                state = event
    assert state.get('key_saved') and state.get('secure_storage'), 'No saved key'
    assert not state.get('active'), 'Existing session is active'
    print('Persistent key present; starting live test.', flush=True)
    previous = None
    diagnostics = {}
    send(b'G')
    began = last = time.monotonic()
    try:
        while time.monotonic() - began < 50:
            if time.monotonic() - last > 2:
                send(b'N'); send(b'D'); last = time.monotonic()
            for event in events():
                category = event.get('type')
                if category == 'voice_diagnostics':
                    diagnostics = event
                if category == 'device_state':
                    status = (event.get('active'), event.get('message'))
                    if status != previous:
                        print('Status:', status, flush=True)
                        previous = status
                    if not event.get('active') and time.monotonic() - began > 5:
                        raise RuntimeError('Live session ended early')
    finally:
        send(b'Z')
        deadline = time.monotonic() + 20
        last = 0
        finished = False
        while time.monotonic() < deadline and not finished:
            if time.monotonic() - last > 1:
                send(b'N'); send(b'D'); last = time.monotonic()
            for event in events():
                if event.get('type') == 'voice_diagnostics':
                    diagnostics = event
                if event.get('type') == 'device_state' and not event.get('active'):
                    print('Closed:', {k:event.get(k) for k in ('message','finalized','seconds')}, flush=True)
                    finished = True
        print('Diagnostics:', diagnostics, flush=True)
