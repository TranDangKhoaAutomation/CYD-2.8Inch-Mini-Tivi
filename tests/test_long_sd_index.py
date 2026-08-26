from pathlib import Path
import unittest

SRC = Path(__file__).resolve().parents[1] / 'src' / 'main.cpp'

class LongSdIndexRegression(unittest.TestCase):
    def test_index_is_streamed_from_sd_not_fully_malloced(self):
        s = SRC.read_text(encoding='utf-8')
        self.assertNotIn('malloc((ix.frames + 1) * sizeof(uint32_t))', s)
        self.assertIn('readIndexOffset', s)
        self.assertIn('File idxFile', s)

if __name__ == '__main__':
    unittest.main()