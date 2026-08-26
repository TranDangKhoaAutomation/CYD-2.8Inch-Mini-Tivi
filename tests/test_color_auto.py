from pathlib import Path
import unittest
SRC = Path(__file__).resolve().parents[1] / 'src' / 'main.cpp'

class ColorAutoRegression(unittest.TestCase):
    def test_firmware_has_auto_color_calibration(self):
        s = SRC.read_text(encoding='utf-8')
        self.assertIn('runJPEGColorAutoTest', s)
        self.assertIn('colorauto', s)
        self.assertIn('readPixel', s)
        self.assertIn("applyJPEGColorMode(bestMode)", s)

if __name__ == '__main__':
    unittest.main()