from pathlib import Path
import unittest
SRC = (Path(__file__).resolve().parents[1]/'src'/'main.cpp').read_text(encoding='utf-8')

class JpegRbSwapRegression(unittest.TestCase):
    def test_video_jpeg_callback_swaps_red_blue_channels(self):
        self.assertIn('swapRB565', SRC)
        self.assertIn('jpegSwapRB', SRC)
        self.assertIn('rbswap on', SRC)
        self.assertIn('rbswap off', SRC)
        body = SRC[SRC.index('static int jpegDraw'):SRC.index('static void applyJPEGColorMode')]
        self.assertIn('swapRB565', body)

if __name__ == '__main__':
    unittest.main()