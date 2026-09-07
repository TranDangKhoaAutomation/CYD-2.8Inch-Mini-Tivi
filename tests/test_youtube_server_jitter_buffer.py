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

def test_resolver_prefers_low_bandwidth_source_and_accepts_live_hls():
    assert 'bestvideo[height<=480][ext=mp4][vcodec^=avc1][protocol=https]+bestaudio[protocol=https]' in SRC
    assert '[protocol=https]' in SRC.split('def resolve_direct_streams', 1)[1].split('def resolve_direct_media', 1)[0]
    assert 'bestvideo[height<=720]' not in SRC

def test_server_terminates_ffmpeg_on_sustained_video_underrun():
    assert 'VIDEO_STALL_RESTART_SEC = 12.0' in SRC
    gen = SRC.split('def mjpeg_generator():', 1)[1].split('def thumb_cache_path', 1)[0]
    assert 'stalled_source = True' in gen
    assert 'video stall' in gen
    assert 'proc.terminate()' in gen
    assert 'refresh_youtube_sources' in gen
    assert 'non-live video stalled -> close HTTP stream' not in gen
    assert 'resume_seconds' in gen

def test_youtube_refresh_keeps_same_http_stream_and_seeks_forward():
    assert 'def refresh_youtube_sources(' in SRC
    assert 'youtube_refresh_lock' in SRC
    assert 'resume_seconds' in SRC
    video = SRC.split('def mjpeg_generator():',1)[1].split('def thumb_cache_path',1)[0]
    audio = SRC.split('def audio_generator():',1)[1].split('@app.post("/play")',1)[0]
    assert 'refresh_youtube_sources(' in video
    assert 'refresh_youtube_sources(' in audio
    assert 'build_video_command(direct, resume_seconds if is_youtube_session else 0.0)' in video
    assert 'build_audio_command(audio_direct, resume_seconds if is_youtube_session else 0.0)' in audio
    youtube_branch = video.split('if snap.get("source_kind") == "youtube":',1)[1].split('if proc and proc.poll()',1)[0]
    assert 'break' not in youtube_branch

def test_direct_resolver_is_serialized_and_bounded_for_fast_open():
    assert 'youtube_resolve_lock' in SRC
    resolver = SRC.split('def resolve_selected()',1)[1].split('def refresh_live_source',1)[0]
    assert 'with youtube_resolve_lock:' in resolver
    direct = SRC.split('def resolve_direct_streams',1)[1].split('def resolve_direct_media',1)[0]
    assert '--socket-timeout' in direct
    assert 'timeout=35' in direct

def test_old_media_generation_cannot_mutate_new_session_state():
    assert '"generation": 0' in SRC
    assert 'state["generation"] = generation' in SRC
    video = SRC.split('def mjpeg_generator():',1)[1].split('def thumb_cache_path',1)[0]
    audio = SRC.split('def audio_generator():',1)[1].split('@app.post("/play")',1)[0]
    assert 'state.get("generation") == sync_generation' in video
    assert 'state.get("generation") == sync_generation' in audio
    assert 'stale generation' in video
    assert 'stale generation' in audio
    assert 'state["resume_seconds"] = resume_seconds' in video
    assert video.index('state.get("generation") == sync_generation') < video.index('state["resume_seconds"] = resume_seconds')


def test_resume_seek_is_youtube_only_not_live_hls():
    video = SRC.split('def mjpeg_generator():',1)[1].split('def thumb_cache_path',1)[0]
    audio = SRC.split('def audio_generator():',1)[1].split('@app.post("/play")',1)[0]
    assert 'is_youtube_session' in video
    assert 'is_youtube_session' in audio
    assert 'if is_youtube_session else 0.0' in video
    assert 'if is_youtube_session else 0.0' in audio
    assert 'if is_youtube_session:' in video


if __name__ == '__main__':
    test_server_has_deep_frame_jitter_buffer()
    test_ffmpeg_is_not_real_time_throttled()
    test_resolver_prefers_low_bandwidth_source_and_accepts_live_hls()
    test_server_terminates_ffmpeg_on_sustained_video_underrun()
    test_youtube_refresh_keeps_same_http_stream_and_seeks_forward()
    test_direct_resolver_is_serialized_and_bounded_for_fast_open()
    test_old_media_generation_cannot_mutate_new_session_state()
    test_resume_seek_is_youtube_only_not_live_hls()
    print('PASS')
