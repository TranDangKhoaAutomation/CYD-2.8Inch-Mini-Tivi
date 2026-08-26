from pathlib import Path

MAIN = Path("src/main.cpp").read_text(encoding="utf-8")
UI = Path("src/display/ui_draw.cpp").read_text(encoding="utf-8")
FONT = Path("src/display/vietnamese_font.h").read_text(encoding="utf-8")

def test_keyboard_is_phone_qwerty():
    assert 'QWERTYUIOP' in MAIN
    assert 'ASDFGHJKL' in MAIN
    assert 'ZXCVBNM' in MAIN
    assert 'KeyboardKey' in MAIN
    assert 'hitKeyboardKey' in MAIN
    assert 'SPACE' in MAIN and 'TÌM' in MAIN

def test_font_has_native_atlases_for_ui_sizes():
    for token in ['VI_FONT_1', 'VI_FONT_2', 'VI_FONT_3', 'VI_FONT_4']:
        assert token in FONT
    assert 'fontForSize' in UI
    assert '* scale' not in UI.split('void drawUtf8Text',1)[1].split('} // namespace',1)[0]

if __name__ == '__main__':
    test_keyboard_is_phone_qwerty()
    test_font_has_native_atlases_for_ui_sizes()
    print('PASS')
