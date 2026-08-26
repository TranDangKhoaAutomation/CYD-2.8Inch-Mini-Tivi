from pathlib import Path

CPP = Path('src/display/display.cpp').read_text(encoding='utf-8')

def test_display_uses_verified_cyd_baseline():
    assert 'Arduino_HWSPI' in CPP
    assert 'Arduino_ILI9341' in CPP
    assert 'SPIClass tftSpi(HSPI)' in CPP
    assert '40000000' in CPP
    assert 'setRotation(1)' in CPP
    assert 'invertDisplay(true)' in CPP
    assert 'draw16bitBeRGBBitmap' in CPP

if __name__ == '__main__':
    test_display_uses_verified_cyd_baseline()
    print('PASS')
