from pathlib import Path

SRC = Path('src/main.cpp').read_text(encoding='utf-8')
CFG = Path('src/config.h').read_text(encoding='utf-8')

def test_touch_uses_isolated_software_spi_and_calibration():
    assert 'touchRead12' in SRC
    assert 'readTouchRaw' in SRC
    assert 'runTouchCalibration' in SRC
    assert 'cydtouch' in SRC
    for token in ['TOUCH_CLK  25', 'TOUCH_CS   33', 'TOUCH_DIN  32', 'TOUCH_DO   39', 'TOUCH_IRQ  36']:
        assert token in CFG

if __name__ == '__main__':
    test_touch_uses_isolated_software_spi_and_calibration()
    print('PASS')
