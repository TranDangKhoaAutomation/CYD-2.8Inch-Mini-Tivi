from pathlib import Path
import unittest
ROOT = Path(__file__).resolve().parents[1]

class ReferenceColorPath(unittest.TestCase):
    def test_matches_verified_cyd_video_path(self):
        ini = (ROOT/'platformio.ini').read_text(encoding='utf-8')
        src = (ROOT/'src'/'main.cpp').read_text(encoding='utf-8')
        disp = (ROOT/'src'/'display'/'display.cpp').read_text(encoding='utf-8')
        self.assertIn('JPEGDEC.git#1.8.2', ini)
        self.assertIn("applyJPEGColorMode('A')", src)
        self.assertIn('invertDisplay(true)', disp)
        self.assertIn('draw16bitBeRGBBitmap', disp)

if __name__ == '__main__': unittest.main()
