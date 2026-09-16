import unittest
import asyncio
import json
import queue
import threading
from websockets.asyncio.server import serve
from home_voice.usb import BoardLink, Decoder, encode_packet
from unittest.mock import Mock


class USBTests(unittest.TestCase):
    def test_wake_settings_validation_and_ack(self):
        board=BoardLink.__new__(BoardLink)
        board.wake_lock=threading.Lock();board.wake_ack=Mock();board.wake_ack.wait.return_value=True
        commands=[]
        def command(value):
            commands.append(value);board.wake_ok=True
        board.command=command
        board.configure_wake({'enabled':True,'phrase':0,'sensitivity':50})
        self.assertEqual(commands,[b'W\x01\x00\x32',b'Q'])
        commands.clear();board.configure_wake({'testing':True})
        self.assertEqual(commands,[b'W\x02',b'Q'])
        for data in ({'testing':1},{'enabled':True,'phrase':2,'sensitivity':50},{'enabled':1,'phrase':0,'sensitivity':50},{'enabled':True,'phrase':0,'sensitivity':101}):
            with self.assertRaises(ValueError):board.configure_wake(data)
        board.wake_ack.wait.return_value=False
        with self.assertRaises(RuntimeError):board.configure_wake({'testing':False})

    def test_key_save_requires_persistent_storage_and_ack(self):
        board=BoardLink.__new__(BoardLink)
        board.native={"secure_storage":False}
        board.command=Mock()
        with self.assertRaises(ValueError):board.save_key('test-placeholder-not-a-real-key')
        board.command.assert_not_called()
        board.native={"secure_storage":True}
        board.key_lock=threading.Lock()
        board.key_ack=Mock()
        board.key_ack.wait.return_value=False
        with self.assertRaises(RuntimeError):board.save_key('test-placeholder-not-a-real-key')
        board.key_ack.wait.return_value=True
        board.command=lambda data:setattr(board,'key_save_ok',True)
        board.save_key('test-placeholder-not-a-real-key')
        board.command=lambda data:setattr(board,'key_save_ok',False)
        with self.assertRaises(RuntimeError):board.save_key('test-placeholder-not-a-real-key')

    def test_volume_requires_device_acknowledgment(self):
        board=BoardLink.__new__(BoardLink)
        board.volume=40
        board.volume_ack=threading.Event()
        board.volume_lock=threading.Lock()
        commands=[]
        def command(data):
            commands.append(data);board.volume=data[1];board.volume_ack.set()
        board.command=command
        board.set_volume(60)
        self.assertEqual(commands,[b'V\x3c'])
        self.assertEqual(board.volume,60)
        for invalid in (-1,81,True,40.5,'60'):
            with self.assertRaises(ValueError):board.set_volume(invalid)
        board.volume_ack=Mock()
        board.volume_ack.wait.return_value=False
        board.command=lambda data:None
        with self.assertRaises(RuntimeError):board.set_volume(0)

    def test_split_and_coalesced_packets(self):
        wire = encode_packet(2, bytes(640)) + encode_packet(3, b'{"type":"device"}')
        decoder = Decoder()
        packets = []
        for index in range(0, len(wire), 7):
            packets.extend(decoder.feed(wire[index:index+7]))
        self.assertEqual(packets, [(2, bytes(640)), (3, b'{"type":"device"}')])

    def test_recover_from_boot_noise_and_invalid_length(self):
        decoder = Decoder()
        wire = b'boot\r\nHV\x02\xff\xffgarbage' + encode_packet(3, b'{}')
        self.assertEqual(decoder.feed(wire), [(3, b'{}')])
        decoder.feed(b'x' * 100000)
        self.assertLessEqual(len(decoder.buffer), 4)


class RelayTests(unittest.IsolatedAsyncioTestCase):
    def board(self):
        board = BoardLink.__new__(BoardLink)
        board.info = {"firmware":"test"}
        board.stop = threading.Event()
        board.active = threading.Event()
        board.audio = queue.Queue(maxsize=25)
        board.error = ""
        board.result = {}
        board.commands = []
        board.output = []
        def command(value):
            board.commands.append(value)
            if value == b'S': board.audio.put_nowait(bytes(640))
        board.command = command
        board.send = lambda kind, payload: board.output.append((kind,payload))
        return board

    async def test_ready_audio_and_finalization(self):
        board = self.board()
        async def provider(socket):
            self.assertEqual(json.loads(await socket.recv())["type"],"start")
            self.assertNotIn(b'S',board.commands)
            await socket.send('{"type":"ready"}')
            self.assertEqual(await socket.recv(),bytes(640))
            await socket.send(bytes(320))
            await socket.send('{"type":"closed","finalized":true,"input_bytes":640}')
        async with serve(provider,"127.0.0.1",0) as server:
            port=server.sockets[0].getsockname()[1]
            await asyncio.wait_for(board.converse(f'ws://127.0.0.1:{port}/audio'),3)
        self.assertEqual(board.output,[(2,bytes(320))])
        self.assertEqual(board.commands,[b'S',b'X'])
        self.assertFalse(board.active.is_set())
        self.assertTrue(board.result['finalized'])

    async def test_stop_waits_for_terminal_usage(self):
        board = self.board()
        async def provider(socket):
            await socket.recv()
            await socket.send('{"type":"ready"}')
            await socket.recv()
            board.stop.set()
            self.assertEqual(json.loads(await socket.recv())["type"],"stop")
            await socket.send('{"type":"closed","finalized":true,"usage":{"seconds":1.2}}')
        async with serve(provider,"127.0.0.1",0) as server:
            port=server.sockets[0].getsockname()[1]
            await asyncio.wait_for(board.converse(f'ws://127.0.0.1:{port}/audio'),3)
        self.assertEqual(board.result['usage']['seconds'],1.2)
        self.assertFalse(board.active.is_set())
