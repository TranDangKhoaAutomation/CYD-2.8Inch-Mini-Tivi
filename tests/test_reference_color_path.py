from pathlib import Path
import unittest
ROOT = Path(__file__).resolve().parents[1]

class ReferenceColorPath(unittest.TestCase):
    def test_matches_known_good_cyd_slideshow_path(self):
        ini = (ROOT/'platformio.ini').read_text(encoding='utf-8')
        src = (ROOT/'src'/'main.cpp').read_text(encoding='utf-8')
        self.assertIn('https://github.com/bitbank2/JPEGDEC.git#1.7.0', ini)
        setup = src[src.index('void setup() {'):src.index('void loop() {')]
        self.assertIn("applyJPEGColorMode('D')", setup)
        self.assertNotIn('runJPEGColorAutoTest();', setup)

if __name__ == '__main__':
    unittest.main()