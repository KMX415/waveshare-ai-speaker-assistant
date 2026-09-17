"""Verify memory persistence and deletion without printing personal contents.

Stop the PC setup service first. --enable-auto enables the user's requested
automatic memory preference. --live also tests paid model tool calls and speaks
two brief confirmations. Do not converse with the board during that test.
"""
import argparse
import json
import time
from home_voice.usb import Decoder, encode_packet, open_board

parser = argparse.ArgumentParser()
parser.add_argument('--enable-auto', action='store_true')
parser.add_argument('--live', action='store_true')
args = parser.parse_args()
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

started = False
fixture_owned = False
try:
    poll(2)
    assert not reports['device_state']['active'], 'User conversation active'
    assert not reports['memory_diagnostics']['test_present'], 'Test fixture already present; inspect before running'
    diagnostic(4)
    print('PASS: invalid names, units and memories rejected; saved data unchanged', flush=True)
    if args.enable_auto:
        assert diagnostic(1)['auto_memory']
    assert diagnostic(2)['test_present']
    fixture_owned = True
    send(b'R')
    board.close()
    time.sleep(8)
    board = open_board('COM14')
    decoder = Decoder()
    reports.clear()
    wait(lambda: 'memory_diagnostics' in reports)
    assert reports['memory_diagnostics']['test_present']
    if args.enable_auto:
        assert reports['memory_diagnostics']['auto_memory']
    print('PASS: saved memory and automatic-memory preference survived reboot', flush=True)
    assert not diagnostic(3)['test_present']
    print('PASS: saved test memory removed', flush=True)
    if args.live:
        send(b'G');started = True
        wait(lambda: reports['device_state']['message'] == 'Listening', 45)
        send(b'YRemember the exact value temporary firmware check under the exact memory key firmware_memory_test. This is a temporary test, not a personal preference. Briefly confirm after saving.')
        wait(lambda: reports['memory_diagnostics']['test_present'], 60)
        poll(5)
        print('PASS: live assistant saved the requested memory through its tool', flush=True)
        send(b'YForget the memory with key firmware_memory_test. Briefly confirm after deleting it.')
        wait(lambda: not reports['memory_diagnostics']['test_present'], 60)
        poll(5)
        assert reports['upload_status']['functions'] >= 2
        print('PASS: live assistant deleted the saved memory through its tool', flush=True)
finally:
    if started:
        send(b'Z')
        wait(lambda: not reports['device_state']['active'], 25)
        assert reports['device_state']['finalized']
    # Remove only the known temporary fixture, even if the live tool test failed.
    if board.is_open and fixture_owned:
        try:
            diagnostic(3)
        finally:
            board.close()
    elif board.is_open:
        board.close()
