from pathlib import Path
import unittest
SRC = (Path(__file__).resolve().parents[1] / 'src' / 'main.cpp').read_text(encoding='utf-8')

class ColorPathRegression(unittest.TestCase):
    def test_firmware_uses_fixed_verified_color_path(self):
        self.assertIn('RGB565_BIG_ENDIAN', SRC)
        self.assertIn('jpegDrawCallback', SRC)
        self.assertIn('fixed verified path', SRC)

if __name__ == '__main__': unittest.main()
