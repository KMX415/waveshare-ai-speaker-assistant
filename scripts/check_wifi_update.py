"""Install the explicitly selected signed preview on an idle, migrated speaker.

USB carries status/control only; the board fetches its own firmware over HTTPS.
Confirms preservation and a second reboot so pending-verify firmware cannot be
mistaken for an accepted update. Does not start paid OpenAI sessions.
"""
import argparse
import json
import time
import serial
from home_voice.usb import Decoder, encode_packet, open_board

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port', default='COM14')
parser.add_argument('--expected-release', required=True, type=int)
args = parser.parse_args()
board = open_board(args.port)
decoder = Decoder()
reports = {}
last_update_state = None


def send(command):
    board.write(encode_packet(1, command))


def poll():
    global board, decoder, last_update_state
    try:
        for command in (b'u', b'N', b'h', b'O\0', b'M'):
            send(command)
        for kind, payload in decoder.feed(board.read(2048)):
            if kind != 3:
                continue
            try:
                event = json.loads(payload)
            except (ValueError, UnicodeDecodeError):
                continue
            reports[event.get('type')] = event
            if event.get('type') == 'update_status' and event.get('state') != last_update_state:
                last_update_state = event.get('state')
                print('Update:', event.get('release'), last_update_state, flush=True)
    except (serial.SerialException, OSError):
        board.close()
        time.sleep(1)
        try:
            board = open_board(args.port)
            decoder = Decoder()
            reports.clear()
        except (serial.SerialException, OSError):
            pass
    time.sleep(.2)


def wait(predicate, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        poll()
        if predicate():
            return
    raise RuntimeError('Timed out: ' + str(reports.get('update_status', {})))


required = {'update_status', 'device_state', 'device_identity', 'memory_diagnostics', 'microphone_status'}
try:
    wait(lambda: required <= reports.keys(), 15)
    assert not reports['device_state']['active'], 'User conversation active'
    assert reports['update_status']['supported'], 'USB migration required'
    assert reports['update_status']['release'] < args.expected_release, 'Expected release must be newer'
    original = dict(identity=reports['device_identity'], key_saved=reports['device_state']['key_saved'],
                    voice=reports['memory_diagnostics']['voice'], automatic_memory=reports['memory_diagnostics']['auto_memory'],
                    gain=reports['microphone_status']['gain_db'], automatic_gain=reports['microphone_status']['automatic'])
    send(b'u\x05')
    print('Preview installation requested; firmware bytes travel over Wi-Fi', flush=True)
    def upgraded():
        update = reports.get('update_status', {})
        if update.get('release') == args.expected_release:
            return True
        state = update.get('state', '')
        if not update.get('busy') and not update.get('queued') and any(
                word in state.lower() for word in ('failed', 'unavailable', 'retry later', 'does not fit', 'already up to date')):
            raise RuntimeError('Device stopped update safely: ' + str(update))
        return False
    wait(upgraded, 240)
    wait(lambda: required <= reports.keys() and not reports['update_status']['busy'], 45)
    assert reports['device_identity'] == original['identity']
    assert reports['device_state']['key_saved'] == original['key_saved']
    assert reports['memory_diagnostics']['voice'] == original['voice']
    assert reports['memory_diagnostics']['auto_memory'] == original['automatic_memory']
    assert reports['microphone_status']['gain_db'] == original['gain']
    assert reports['microphone_status']['automatic'] == original['automatic_gain']
    print('PASS: Wi-Fi update booted and retained identity, encrypted key and preferences', flush=True)
    send(b'RRESTART')
    board.close()
    time.sleep(8)
    board = open_board(args.port)
    decoder = Decoder()
    reports.clear()
    wait(lambda: required <= reports.keys(), 20)
    assert reports['update_status']['release'] == args.expected_release, 'Boot validation did not accept the update'
    assert reports['device_identity'] == original['identity']
    assert reports['device_state']['key_saved'] == original['key_saved']
    print('PASS: second reboot retained the new release; OTA boot was accepted', flush=True)
finally:
    board.close()
