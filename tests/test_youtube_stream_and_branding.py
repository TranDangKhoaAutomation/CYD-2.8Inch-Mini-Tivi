from pathlib import Path

SRC = Path("src/main.cpp").read_text(encoding="utf-8")
SERVER = Path("tools/youtube_tv_server/server.py").read_text(encoding="utf-8")
BITMAP = Path("src/display/brand_name_bitmap.h")

def test_youtube_stream_is_capped_for_esp32_capacity():
    assert 'YT_STREAM_FPS = 10' in SERVER
    assert 'fps={YT_STREAM_FPS}' in SERVER

def test_youtube_stream_uses_high_quality_scaling():
    assert 'YT_STREAM_Q = 5' in SERVER
    assert 'flags=lanczos' in SERVER
    assert 'flags=fast_bilinear' not in SERVER


def test_youtube_thumbnail_matches_firmware_slot_and_title_is_pixel_fitted():
    assert 'img.thumbnail((96, 54), Image.Resampling.LANCZOS)' in SERVER
    assert 'Image.new("RGB", (96, 54), "black")' in SERVER
    assert '_96x54.jpg' in SERVER
    assert 'YT_THUMB_W = 96' in SRC and 'YT_THUMB_H = 54' in SRC
    assert 'fitUtf8PrefixBytes' in SRC and 'fitTextPx' in SRC
    assert 'textWidth' in Path('src/display/ui_draw.h').read_text(encoding='utf-8')
    assert 'Old server caches were 112x63' in SRC

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


def test_discovery_uses_single_8876_server_port():
    main = SRC
    assert "YT_SERVER_PORT = 8876" in main
    assert "8765" not in main
    assert "validateServer(ip, YT_SERVER_PORT" in main

if __name__ == '__main__':
    for name, fn in list(globals().items()):
        if name.startswith('test_') and callable(fn): fn()
    print('PASS')
