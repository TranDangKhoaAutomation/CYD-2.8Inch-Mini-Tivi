from pathlib import Path
import unittest
SRC = (Path(__file__).resolve().parents[1]/'src'/'main.cpp').read_text(encoding='utf-8')

class JpegPipelineRegression(unittest.TestCase):
    def test_video_callback_does_not_software_swap_rb(self):
        body = SRC[SRC.index('static int jpegDraw'):SRC.index('static void applyJPEGColorMode')]
        self.assertIn('jpegDrawCallback', body)
        self.assertNotIn('swapRB565', body)

if __name__ == '__main__': unittest.main()
