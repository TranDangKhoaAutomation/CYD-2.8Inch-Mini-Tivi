from pathlib import Path

SRC = Path('src/main.cpp').read_text(encoding='utf-8')

def test_touch_calibration_runtime_is_present():
    assert 'TouchCalibration' in SRC
    assert 'cydtouch' in SRC
    assert 'runTouchCalibration' in SRC
    assert 'touchcal' in SRC
    assert 'touchCal.rawLeft' in SRC
    assert 'touchCal.rawRight' in SRC
    assert 'touchCal.rawTop' in SRC
    assert 'touchCal.rawBottom' in SRC

if __name__ == '__main__':
    test_touch_calibration_runtime_is_present()
    print('PASS')
