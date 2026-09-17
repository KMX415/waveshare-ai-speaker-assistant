"""Paid voice-selection smoke test. Stop PC setup first; do not converse during test.
Tests tool save, reboot persistence and new-session acceptance, then restores voice.
"""
import json
import time
from home_voice.usb import Decoder, encode_packet, open_board
reports = {}
decoder = Decoder()
board = open_board('COM14')

def send(command):
    board.write(encode_packet(1, command))

def poll(seconds):
    end = time.monotonic() + seconds
    next_poll = 0
    while time.monotonic() < end:
        if time.monotonic() >= next_poll:
            for command in (b'N', b'O\x00', b'U'):
                send(command)
            next_poll = time.monotonic() + .5
        for kind, payload in decoder.feed(board.read(2048)):
            if kind != 3:
                continue
            try:
                event = json.loads(payload)
            except (ValueError, UnicodeDecodeError):
                continue
            reports[event.get('type')] = event

def wait(predicate, seconds=40):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        poll(.5)
        if predicate():
            return
    raise RuntimeError('Timed out waiting for memory test step')

def diagnostic(action):
    send(b'O' + bytes([action]))
    # Read the action response before status polling can replace it.
    end = time.monotonic() + 5
    while time.monotonic() < end:
        for kind, payload in decoder.feed(board.read(2048)):
            if kind == 3:
                event = json.loads(payload)
                if event.get('type') == 'memory_diagnostics' and event.get('action') == action:
                    assert event['ok'], 'Device rejected diagnostic operation'
                    return event
    raise RuntimeError('No memory diagnostic reply')


original = None
try:
    poll(2)
    assert not reports['device_state']['active'], 'User conversation active'
    original = reports['memory_diagnostics']['voice']
    alternate = 'cedar' if original != 'cedar' else 'marin'
    diagnostic(4)
    print('PASS: invalid voice rejected without changing settings or memories', flush=True)
    send(b'G')
    wait(lambda: reports['device_state']['message'] == 'Listening')
    send(b'Y' + f'Change your voice to {alternate}. Use the tool and briefly confirm when it takes effect.'.encode())
    wait(lambda: reports['memory_diagnostics']['voice'] == alternate, 60)
    poll(5)
    send(b'Z'); wait(lambda: not reports['device_state']['active'], 25)
    assert reports['device_state']['finalized']
    print('PASS: model saved alternate voice via tool', flush=True)
    send(b'R'); board.close(); time.sleep(8)
    board = open_board('COM14'); decoder = Decoder(); reports.clear()
    wait(lambda: 'memory_diagnostics' in reports)
    assert reports['memory_diagnostics']['voice'] == alternate
    print('PASS: alternate voice persisted through reboot', flush=True)
    send(b'G'); wait(lambda: reports['device_state']['message'] == 'Listening')
    print('PASS: OpenAI accepted new session using saved alternate voice', flush=True)
finally:
    if original is not None:
        poll(1)
        if not reports['device_state']['active']:
            send(b'G'); wait(lambda: reports['device_state']['message'] == 'Listening')
        send(b'Y' + f'Change your voice back to {original}. Use set_voice and briefly confirm.'.encode())
        wait(lambda: reports['memory_diagnostics']['voice'] == original, 60)
        poll(5)
        send(b'Z'); wait(lambda: not reports['device_state']['active'], 25)
        print('PASS: original voice restored and session closed', flush=True)
    board.close()
