"""Check persisted wake settings and open a detection-only test (no API calls)."""
import time
from home_voice.usb import BoardLink

def wait_for(test, seconds=20):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        if test():return
        time.sleep(.1)
    raise RuntimeError('Device check timed out')

board=BoardLink('COM14')
try:
    wait_for(lambda:board.wake.get('ready'))
    print('Wake model ready:',board.wake,flush=True)
    assert board.native.get('key_saved') and not board.native.get('active')
    board.configure_wake({'enabled':False,'phrase':1,'sensitivity':61})
    wait_for(lambda:board.wake.get('phrase')==1 and board.wake.get('sensitivity')==61)
    board.command(b'RRESTART')
finally:board.close()
time.sleep(3)
board=BoardLink('COM14')
try:
    wait_for(lambda:'enabled' in board.wake)
    assert board.wake['enabled'] is False and board.wake['phrase']==1 and board.wake['sensitivity']==61
    assert board.native.get('key_saved')
    print('PASS: wake preferences and encrypted API key survived restart.',flush=True)
    board.configure_wake({'enabled':False,'phrase':0,'sensitivity':50})
    board.configure_wake({'testing':True})
    wait_for(lambda:board.wake.get('testing') and board.wake.get('ready') and board.wake.get('phrase')==0)
    board.command(b'G');time.sleep(1);board.command(b'N');time.sleep(.3)
    assert not board.native.get('active')
    print('PASS: detection-only mode blocks live sessions.',flush=True)
    print('Jarvis detection test ready:',board.wake,flush=True)
finally:board.close()
