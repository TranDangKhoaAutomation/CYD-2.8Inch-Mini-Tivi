from pathlib import Path
import unittest
ROOT = Path(__file__).resolve().parents[1]

class DisplayStackRegression(unittest.TestCase):
    def test_no_legacy_tft_rgb_order_flag(self):
        ini = (ROOT/'platformio.ini').read_text(encoding='utf-8')
        self.assertIn('Arduino_GFX', ini)
        self.assertNotIn('TFT_RGB_ORDER', ini)
        self.assertNotIn('TFT_eSPI', ini)

if __name__ == '__main__': unittest.main()
