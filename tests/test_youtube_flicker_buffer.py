from pathlib import Path

SRC = Path("src/main.cpp").read_text(encoding="utf-8")

def test_youtube_uses_full_frame_buffer_before_present():
    assert 'YT_FRAME_W = 320' in SRC
    assert 'YT_FRAME_H = 180' in SRC
    assert 'ytFramePart' in SRC
    assert 'YT_FRAME_PART_H = 90' in SRC
    assert 'copyYTDecodedBlock' in SRC
    assert 'presentYTFrame' in SRC
    assert 'YT_FRAME_PARTS = 2' in SRC

def test_youtube_network_buffer_is_not_forced_to_64k():
    yt_reader = SRC.split('static int readNextYTFrame()',1)[1].split('static bool startYTDebugStream()',1)[0]
    assert 'ensureBuf(64 * 1024)' not in yt_reader
    assert 'YT_JPEG_INITIAL_BUF = 16 * 1024' in SRC

def test_youtube_footer_is_simple():
    assert 'YouTube via LAN' not in SRC
    assert 'YouTube qua LAN' not in SRC
    assert 'VIDEO ONLY' not in SRC
    assert 'CHỈ VIDEO' not in SRC
    assert 'ui().text("YouTube", 160, 228, 2);' in SRC

if __name__ == '__main__':
    test_youtube_uses_full_frame_buffer_before_present()
    test_youtube_network_buffer_is_not_forced_to_64k()
    test_youtube_footer_is_simple()
    print('PASS')
