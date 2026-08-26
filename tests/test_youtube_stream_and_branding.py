from pathlib import Path

SRC = Path("src/main.cpp").read_text(encoding="utf-8")
SERVER = Path("tools/youtube_tv_server/server.py").read_text(encoding="utf-8")
BITMAP = Path("src/display/brand_name_bitmap.h")

def test_youtube_stream_is_capped_for_esp32_capacity():
    assert 'YT_STREAM_FPS = 6' in SERVER
    assert 'fps={YT_STREAM_FPS}' in SERVER

def test_default_feed_is_not_music_robot_query():
    assert 'music technology robots' not in SRC
    assert 'music technology robots' not in SERVER
    assert 'Tran Dang Khoa' in SRC or 'Trần Đăng Khoa' in SRC

def test_vietnamese_brand_uses_bitmap_renderer():
    assert Path('src/display/vietnamese_font.h').exists()
    assert 'drawUtf8Text' in Path('src/display/ui_draw.cpp').read_text(encoding='utf-8')
    assert 'Trần Đăng Khoa' in SRC

def test_search_is_exposed():
    assert 'YT_KEYBOARD' in SRC
    assert '/api/search?q=' in SRC
    assert 'Tìm kiếm video' in SRC or 'TÌM' in SRC

if __name__ == '__main__':
    for f in [test_youtube_stream_is_capped_for_esp32_capacity, test_default_feed_is_not_music_robot_query, test_vietnamese_brand_uses_bitmap_renderer, test_search_is_exposed]: f()
    print('PASS')
