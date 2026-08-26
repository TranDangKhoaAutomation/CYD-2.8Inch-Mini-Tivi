from pathlib import Path

MAIN = Path("src/main.cpp").read_text(encoding="utf-8")
UIH = Path("src/display/ui_draw.h").read_text(encoding="utf-8")
UIC = Path("src/display/ui_draw.cpp").read_text(encoding="utf-8")
FONT = Path("src/display/vietnamese_font.h")

def test_language_state_and_persistence_exist():
    assert 'enum class UiLanguage' in MAIN
    assert 'cydlang' in MAIN
    assert 'saveLanguage' in MAIN
    assert 'loadLanguage' in MAIN

def test_home_exposes_both_language_choices():
    assert 'TIẾNG VIỆT' in MAIN
    assert 'ENGLISH' in MAIN
    assert 'setLanguage(UiLanguage::VI)' in MAIN
    assert 'setLanguage(UiLanguage::EN)' in MAIN

def test_utf8_renderer_is_global_ui_path():
    assert FONT.exists()
    assert 'drawUtf8Text' in UIC
    assert 'measureUtf8Text' in UIC
    assert 'gfx_->print(s)' not in UIC

def test_bilingual_branding_and_labels():
    assert 'Trần Đăng Khoa' in MAIN
    assert 'Tran Dang Khoa' in MAIN
    assert 'Không có thẻ SD' in MAIN
    assert 'No SD card' in MAIN
    assert 'Tìm kiếm video' in MAIN
    assert 'Search videos' in MAIN

if __name__ == '__main__':
    for f in [test_language_state_and_persistence_exist,test_home_exposes_both_language_choices,test_utf8_renderer_is_global_ui_path,test_bilingual_branding_and_labels]: f()
    print('PASS')
