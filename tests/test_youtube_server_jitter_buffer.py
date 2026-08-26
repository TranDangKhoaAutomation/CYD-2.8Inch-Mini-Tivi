from pathlib import Path

SRC = Path("tools/youtube_tv_server/server.py").read_text(encoding="utf-8")

def test_server_has_deep_frame_jitter_buffer():
    assert 'import queue' in SRC
    assert 'YT_BUFFER_FRAMES' in SRC
    assert 'YT_PREBUFFER_FRAMES' in SRC
    assert 'queue.Queue' in SRC
    assert '_mjpeg_producer' in SRC

def test_ffmpeg_is_not_real_time_throttled():
    cmd = SRC.split('cmd = [',1)[1].split(']',1)[0]
    assert '\"-re\"' not in cmd

def test_resolver_prefers_progressive_low_bandwidth_source():
    assert 'bestvideo[height<=480][ext=mp4][vcodec^=avc1][protocol=https]' in SRC
    assert 'bestvideo[height<=720]' not in SRC

if __name__ == '__main__':
    test_server_has_deep_frame_jitter_buffer()
    test_ffmpeg_is_not_real_time_throttled()
    test_resolver_prefers_progressive_low_bandwidth_source()
    print('PASS')
