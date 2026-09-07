from pathlib import Path

SRC = Path("src/main.cpp").read_text(encoding="utf-8")

def test_youtube_uses_full_frame_buffer_before_present():
    assert 'YT_FRAME_W = 320' in SRC
    assert 'YT_FRAME_H = 180' in SRC
    assert 'ytFramePart' in SRC
    assert 'YT_FRAME_PART_H = 45' in SRC
    assert 'copyYTDecodedBlock' in SRC
    assert 'presentYTFrame' in SRC
    assert 'YT_FRAME_PARTS = 4' in SRC

def test_youtube_network_buffer_is_not_forced_to_64k():
    yt_reader = SRC.split('static int readNextYTFrame()',1)[1].split('static bool startYTDebugStream()',1)[0]
    assert 'ensureBuf(64 * 1024)' not in yt_reader
    assert 'YT_JPEG_INITIAL_BUF = 16 * 1024' in SRC

def test_youtube_footer_is_simple():
    assert 'YouTube via LAN' not in SRC
    assert 'YouTube qua LAN' not in SRC
    assert 'VIDEO ONLY' not in SRC
    assert 'CHỈ VIDEO' not in SRC
    assert 'ui().text(remoteSourceLabel, 160, 228, 2);' in SRC


def test_youtube_framebuffer_does_not_starve_wifi_or_audio_at_boot():
    setup = SRC.split('void setup() {', 1)[1].split('void loop() {', 1)[0]
    assert 'allocateYTFrameBuffer();' not in setup
    alloc = SRC.split('static bool allocateYTFrameBuffer()', 1)[1].split('static void presentYTFrame()', 1)[0]
    assert 'MEDIA_HEAP_RESERVE = 80 * 1024' in alloc
    assert 'skip full frame' in alloc


def test_youtube_has_bounded_no_frame_watchdog():
    assert 'YT_VIDEO_START_GRACE_MS = 18000' in SRC
    assert 'YT_VIDEO_STALL_MS = 22000' in SRC
    assert 'LIVE_VIDEO_STALL_MS = 8000' in SRC
    assert 'serviceYTVideoHealth()' in SRC
    assert 'no complete frame' in SRC
    assert 'restartRemoteVideoOnly' in SRC
    health = SRC.split('static bool serviceYTVideoHealth()',1)[1].split('static void drawYTOSD()',1)[0]
    assert 'if (remoteReturnScreen == Screen::TV_BROWSER)' not in health
    assert 'Video stalled - reopen it' not in SRC
    assert 'restartRemoteVideoOnly("no complete frame")' in health
    assert 'isLiveTV ? LIVE_VIDEO_STALL_MS : YT_VIDEO_STALL_MS' in health

if __name__ == '__main__':
    test_youtube_uses_full_frame_buffer_before_present()
    test_youtube_network_buffer_is_not_forced_to_64k()
    test_youtube_footer_is_simple()
    test_youtube_framebuffer_does_not_starve_wifi_or_audio_at_boot()
    test_youtube_has_bounded_no_frame_watchdog()
    print('PASS')
