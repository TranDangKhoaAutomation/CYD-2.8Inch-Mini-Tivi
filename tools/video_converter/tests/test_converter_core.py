import struct
import tempfile
import unittest
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from converter_core import STABLE_PRESET, build_index, verify_mjpeg, build_ffmpeg_video_args, parse_ffmpeg_time


class ConverterCoreTests(unittest.TestCase):
    def test_stable_preset_targets_cyd(self):
        self.assertEqual(STABLE_PRESET.width, 320)
        self.assertEqual(STABLE_PRESET.height, 240)
        self.assertEqual(STABLE_PRESET.fps, 12.0)
        self.assertEqual(STABLE_PRESET.quality, 10)

    def test_build_index_writes_cyd1_offsets(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            mjpeg = td / 'x.mjpeg'
            idx = td / 'x.idx'
            f1 = b'\xff\xd8abc\xff\xd9'
            f2 = b'\xff\xd8defgh\xff\xd9'
            mjpeg.write_bytes(f1 + f2)
            info = build_index(mjpeg, idx, 12.0)
            raw = idx.read_bytes()
            self.assertEqual(raw[:4], b'CYD1')
            fps_milli, frames = struct.unpack_from('<II', raw, 4)
            offsets = struct.unpack_from('<III', raw, 12)
            self.assertEqual(fps_milli, 12000)
            self.assertEqual(frames, 2)
            self.assertEqual(offsets, (0, len(f1), len(f1) + len(f2)))
            self.assertEqual(info.frames, 2)

    def test_verify_mjpeg_rejects_truncated_frame(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / 'bad.mjpeg'
            p.write_bytes(b'\xff\xd8broken')
            with self.assertRaises(ValueError):
                verify_mjpeg(p)

    def test_ffmpeg_args_force_esp32_friendly_mjpeg(self):
        args = build_ffmpeg_video_args(Path('in.mp4'), Path('out.mjpeg'), STABLE_PRESET, 'ffmpeg')
        joined = ' '.join(map(str, args))
        self.assertIn('fps=12', joined)
        self.assertIn('scale=320:240', joined)
        self.assertIn('format=yuvj420p', joined)
        self.assertIn('-c:v mjpeg', joined)
        self.assertIn('-q:v 10', joined)
        self.assertIn('-f mjpeg', joined)

    def test_parse_ffmpeg_time(self):
        self.assertAlmostEqual(parse_ffmpeg_time('out_time=00:01:02.500000'), 62.5)
        self.assertIsNone(parse_ffmpeg_time('progress=continue'))

if __name__ == '__main__':
    unittest.main()
