from pathlib import Path
import unittest
ROOT = Path(__file__).resolve().parents[1]

class TftRgbOrderRegression(unittest.TestCase):
    def test_ili9341_forces_rgb_order(self):
        ini = (ROOT/'platformio.ini').read_text(encoding='utf-8')
        self.assertIn('-DTFT_RGB_ORDER=TFT_RGB', ini)

if __name__ == '__main__':
    unittest.main()