from pathlib import Path

SRC = Path('src/main.cpp').read_text(encoding='utf-8')

def test_no_tft_espi_symbols_remain():
    forbidden = ['TFT_eSPI', 'setTextDatum', 'drawRoundRect', 'fillRoundRect']
    for token in forbidden:
        assert token not in SRC

if __name__ == '__main__':
    test_no_tft_espi_symbols_remain()
    print('PASS')
