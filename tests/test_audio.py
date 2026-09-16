import struct
import unittest

from home_voice.audio import PlaybackBuffer, duplicate_mono, select_channel


class AudioTests(unittest.TestCase):
    def test_processed_channel_survives_multichannel_capture(self):
        captured = struct.pack("<6h", 100, 900, -20, 101, 901, -21)
        self.assertEqual(select_channel(captured, 3, 0), struct.pack("<2h", 100, 101))
        self.assertEqual(select_channel(captured, 3, 2), struct.pack("<2h", -20, -21))

    def test_stereo_playback_duplicates_each_sample(self):
        self.assertEqual(duplicate_mono(struct.pack("<2h", -100, 200), 2),
                         struct.pack("<4h", -100, -100, 200, 200))

    def test_split_packets_play_in_order_and_silence_pads_shortfall(self):
        audio = PlaybackBuffer()
        audio.append(b"\x01\x02\x03\x04")
        audio.append(b"\x05\x06")
        self.assertEqual(audio.read(2), b"\x01\x02")
        self.assertEqual(audio.read(6), b"\x03\x04\x05\x06\0\0")

    def test_backlog_is_bounded_and_clear_removes_unplayed_speech(self):
        audio = PlaybackBuffer(max_bytes=4)
        audio.append(b"1234")
        with self.assertRaises(BufferError):
            audio.append(b"56")
        audio.clear()
        self.assertEqual(audio.read(4), b"\0" * 4)

    def test_incomplete_audio_is_rejected(self):
        with self.assertRaises(ValueError):
            select_channel(b"123", 2, 0)
        with self.assertRaises(ValueError):
            duplicate_mono(b"1", 2)
        with self.assertRaises(ValueError):
            PlaybackBuffer().append(b"1")
