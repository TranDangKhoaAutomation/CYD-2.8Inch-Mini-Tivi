from pathlib import Path

INI = Path('platformio.ini').read_text(encoding='utf-8')

def test_verified_display_stack_is_selected():
    assert 'GFX Library for Arduino' in INI or 'Arduino_GFX' in INI
    assert 'TFT_eSPI' not in INI
    assert 'SPI_FREQUENCY=40000000' not in INI

if __name__ == '__main__':
    test_verified_display_stack_is_selected()
    print('PASS')
