from pathlib import Path
import importlib.util
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SERVER_PATH = ROOT / "tools" / "youtube_tv_server" / "server.py"
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
CONFIG = (ROOT / "src" / "config.h").read_text(encoding="utf-8")
SELFTEST = (ROOT / "src" / "audio_selftest.cpp").read_text(encoding="utf-8")
PIO = (ROOT / "platformio.ini").read_text(encoding="utf-8")


def load_server():
    name = "minitv_audio_server_test"
    spec = importlib.util.spec_from_file_location(name, SERVER_PATH)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def test_youtube_resolves_video_and_audio_in_one_lookup():
    s = load_server()
    calls = []
    def fake_ytdlp(args, timeout=35):
        calls.append((args, timeout))
        return {
            "title": "Demo",
            "requested_formats": [
                {"url": "https://cdn.test/video.mp4", "vcodec": "avc1.4d401f", "acodec": "none"},
                {"url": "https://cdn.test/audio.m4a", "vcodec": "none", "acodec": "mp4a.40.2"},
            ],
        }
    s._run_ytdlp_json = fake_ytdlp
    video, audio, title = s.resolve_direct_streams("https://youtube.test/watch?v=1")
    assert video == "https://cdn.test/video.mp4"
    assert audio == "https://cdn.test/audio.m4a"
    assert title == "Demo"
    assert len(calls) == 1


def test_selected_stream_pair_is_cached():
    s = load_server()
    calls = []
    s.choose_video("https://youtube.test/watch?v=2", "Cached")
    def fake_resolve(url):
        calls.append(url)
        return "https://cdn.test/v.mp4", "https://cdn.test/a.m4a", "Cached"
    s.resolve_direct_streams = fake_resolve
    assert s.resolve_selected() == s.resolve_selected()
    assert len(calls) == 1


def test_server_exposes_forward_style_mp3_audio():
    s = load_server()
    cmd = s.build_audio_command("https://cdn.test/audio.m4a")
    assert "-vn" in cmd
    assert cmd[cmd.index("-ac") + 1] == "1"
    assert cmd[cmd.index("-ar") + 1] == "48000"
    assert cmd[cmd.index("-c:a") + 1] == "libmp3lame"
    assert cmd[cmd.index("-b:a") + 1] == "64k"
    assert cmd[cmd.index("-f") + 1] == "mp3"
    af = cmd[cmd.index("-af") + 1]
    assert "highpass=f=220" in af
    assert "lowpass=f=6500" in af
    assert "equalizer=f=2800:t=q:w=1:g=3" in af
    assert "loudnorm=I=-8:TP=-2:LRA=4" in af
    assert "-re" not in cmd, 'MP3 path must rely on AudioI2S/TCP backpressure like the working forward build'
    routes = {r.rule for r in s.app.url_map.iter_rules()}
    assert "/stream.mp3" in routes
    assert "/stream.pcm" not in routes
    server_src = SERVER_PATH.read_text(encoding="utf-8")
    assert 'mp3;rate=48000;channels=1;bitrate=64k' in server_src



def test_audio_server_has_pc_side_jitter_buffer():
    server_src = SERVER_PATH.read_text(encoding="utf-8")
    assert 'AUDIO_BUFFER_CHUNKS' in server_src
    assert 'AUDIO_PREBUFFER_CHUNKS_YT = 4' in server_src
    assert 'AUDIO_PREBUFFER_CHUNKS_HLS = 10' in server_src
    assert 'AUDIO_BYTES_PER_SEC' not in server_src
    assert 'HLS_PREBUFFER_FRAMES = 50' in server_src
    assert 'next_audio_send_at' not in server_src
    assert 'yield chunk' in server_src
    assert 'def _audio_producer(' in server_src
    assert 'queue.Queue[bytes]' in server_src
    assert 'threading.Thread(target=_audio_producer' in server_src
    assert 'audio_prebuffer_target = (AUDIO_PREBUFFER_CHUNKS_HLS' in server_src
    assert 'else AUDIO_PREBUFFER_CHUNKS_YT' in server_src
    assert 'while audio_queue.qsize() < audio_prebuffer_target' in server_src
    assert 'audio_queue.get(timeout=' in server_src
    assert 'pending = bytearray()' in server_src
    assert 'while len(pending) >= AUDIO_CHUNK_BYTES' in server_src
    assert 'chunk = bytes(pending[:AUDIO_CHUNK_BYTES])' in server_src

def test_audio_waits_for_first_video_frame_instead_of_fixed_delay():
    server_src = SERVER_PATH.read_text(encoding="utf-8")
    assert 'playback_sync_cond = threading.Condition()' in server_src
    assert '_reset_playback_sync()' in server_src
    assert '_mark_video_started(sync_generation)' in server_src
    assert '_wait_for_video_started(sync_generation, timeout=20.0)' in server_src
    video = server_src.split('def mjpeg_generator():', 1)[1].split('def thumb_cache_path', 1)[0]
    audio = server_src.split('def audio_generator():', 1)[1].split('@app.post("/play")', 1)[0]
    assert video.index('yield b"--frame') < video.index('_mark_video_started(sync_generation)')
    assert audio.index('_wait_for_video_started(sync_generation, timeout=20.0)') < audio.index('yield chunk')
    assert 'time.sleep(1.5)' not in audio


def test_play_endpoints_advertise_mp3_audio():
    s = load_server()
    c = s.app.test_client()
    y = c.post("/api/play", json={"url": "https://youtube.test/watch?v=3", "title": "YT"})
    assert y.status_code == 200
    assert y.get_json()["audio"] == "/stream.mp3"
    s.resolve_vtvgo_source = lambda channel: "https://vtv.test/master.m3u8"
    v = c.post("/api/tv/play", json={"id": "vtv1"})
    assert v.status_code == 200
    assert v.get_json()["audio"] == "/stream.mp3"


def test_firmware_reuses_forward_audioi2s_internal_dac_architecture():
    assert "ESP32-audioI2S.git#2.0.6" in PIO
    assert '#include "Audio.h"' in MAIN or '#include <Audio.h>' in MAIN
    assert "#define USE_EXTERNAL_I2S_DAC 0" in CONFIG
    assert "Audio(!USE_EXTERNAL_I2S_DAC, USE_EXTERNAL_I2S_DAC ? 3 : 2)" in MAIN
    assert "audio->loop();" in MAIN
    assert 'xTaskCreatePinnedToCore(audioTask, "audio"' in MAIN
    assert ', 0);' in MAIN.split('xTaskCreatePinnedToCore(audioTask, "audio"', 1)[1][:180]
    assert '"/stream.mp3"' in MAIN
    assert "audio->connecttohost" in MAIN
    assert "audio->stopSong" in MAIN
    assert "I2S_MODE_DAC_BUILT_IN" not in MAIN
    assert "i2s_write(" not in MAIN
    loop = MAIN.split("void loop() {", 1)[1]
    assert "serviceAudio();" not in loop


def test_internal_dac_does_not_double_eq_audio():
    # Server already applies the speech-band filter; firmware must not add a second EQ stage.
    assert 'audio->setTone(' not in MAIN


def test_audio_task_is_isolated_from_video_loop():
    task = MAIN.split("static void audioTask(void *)", 1)[1].split("static void audioCmd", 1)[0]
    assert "audio->loop();" in task
    assert "vTaskDelay(1);" in task
    assert "xSemaphoreTake(audioMutex" in task
    assert "xSemaphoreGive(audioMutex)" in task




def test_internal_dac_quantizer_uses_dithered_error_feedback():
    assert 'DAC_DITHER_AMPLITUDE = 32' in MAIN
    assert 'static int32_t dacQuantErrorLeft' in MAIN
    assert 'static int32_t dacQuantErrorRight' in MAIN
    assert 'static uint32_t dacDitherStateLeft' in MAIN
    assert 'static uint32_t dacDitherStateRight' in MAIN
    assert 'static int16_t quantizeForInternalDac8(int16_t sample, int32_t &error, uint32_t &rng)' in MAIN
    assert 'void audio_process_i2s(uint32_t *sample, bool *continueI2S)' in MAIN
    cb = MAIN.split('void audio_process_i2s(uint32_t *sample, bool *continueI2S)', 1)[1].split('// Debounced BOOT', 1)[0]
    assert '*continueI2S = true;' in cb
    assert 'quantizeForInternalDac8(left, dacQuantErrorLeft, dacDitherStateLeft)' in cb
    assert 'quantizeForInternalDac8(right, dacQuantErrorRight, dacDitherStateRight)' in cb
    assert 'base + dither' in MAIN
    assert 'error = base - q;' in MAIN


def test_remote_audio_health_watchdog_reconnects_failed_decoder():
    assert 'static void serviceRemoteAudioHealth()' in MAIN
    health = MAIN.split('static void serviceRemoteAudioHealth()', 1)[1].split('static void initBootVolumeButton', 1)[0]
    assert 'Screen::YT_PLAYER' in health
    assert 'audio->isRunning()' in health
    assert 'audio->inBufferFilled()' in health
    assert 'startRemoteAudio();' in health
    assert 'AUDIO_RETRY_MS' in health
    loop = MAIN.split('void loop() {', 1)[1]
    assert 'serviceRemoteAudioHealth();' in loop

def test_remote_audio_start_is_pop_free_and_does_not_prime_before_video():
    start = MAIN.split('static bool startRemoteAudio()',1)[1].split('static bool waitForRemoteAudioPrime',1)[0]
    assert 'audio->setVolume(0);' in start
    assert start.index('audio->setVolume(0);') < start.index('audio->stopSong();') < start.index('audio->connecttohost')
    assert 'static void serviceRemoteAudioFade()' in MAIN
    fade = MAIN.split('static void serviceRemoteAudioFade()',1)[1].split('static bool waitForRemoteAudioPrime',1)[0]
    assert 'audioDecoderReady' in fade
    assert 'audio->setVolume(' in fade
    loop = MAIN.split('void loop() {',1)[1]
    assert 'serviceRemoteAudioFade();' in loop

    for marker in ['if (rc >= 200 && rc < 300)', 'static bool startTVChannel', 'static bool startYTDebugStream']:
        part = MAIN.split(marker,1)[1][:2600]
        assert 'primeRemoteAudio();' not in part


def test_audio_stop_mutes_before_transport_close():
    stop = MAIN.split('static void stopAudio()',1)[1].split('static void adjustVolume',1)[0]
    assert 'muteRemoteAudioForTransition' in stop
    assert stop.index('muteRemoteAudioForTransition') < stop.index('audio->stopSong()')

def test_boot_button_cycles_volume_and_draws_center_popup():
    assert re.search(r"#define\s+BOOT_BUTTON_PIN\s+0\b", CONFIG)
    assert "handleBootVolumeButton" in MAIN
    assert "cycleBootVolume();" in MAIN
    cycle = MAIN.split("static void cycleBootVolume", 1)[1].split("static void serviceRemoteAudioHealth", 1)[0]
    assert "st.volume >= MAX_VOLUME ? 0" in cycle
    assert "audio->setVolume(st.volume)" in cycle
    assert "showVolumePopup();" in cycle
    assert 'ui().text(String("VOL ") + String(st.volume) + "/" + String(MAX_VOLUME)' in MAIN
    assert "serviceVolumePopup();" in MAIN
    assert "VOLUME_POPUP_MS = 1200" in MAIN


def _macro_int(name):
    m = re.search(rf"#define\s+{name}\s+(\d+)\b", CONFIG)
    assert m, f"missing macro {name}"
    return int(m.group(1))


def test_volume_range_uses_full_audioi2s_range_after_hardware_gain_reduction():
    default = _macro_int("DEFAULT_VOLUME")
    maxv = _macro_int("MAX_VOLUME")
    assert default == 21, default
    assert default == maxv, (default, maxv)
    assert maxv == 21, maxv
    assert "boostInternalDacSample" in MAIN
    assert "boosted = (int32_t)sample * 2" in MAIN
    assert "boosted > 32767" in MAIN and "boosted < -32768" in MAIN


def test_sd_mp3_audio_is_restored():
    assert 'String mp3 = base + ".mp3"' in MAIN
    assert 'audioFallback = "/Audio/" + leaf + ".mp3"' in MAIN
    assert 'SD.exists(audioFallback)' in MAIN
    assert "audio->connecttoFS(SD" in MAIN
    assert "audio->pauseResume();" in MAIN


def test_boot_beep_self_test_uses_three_note_max_level_chime():
    setup = MAIN.split("void setup() {", 1)[1].split("void loop() {", 1)[0]
    assert "playBootMaxBeeps(*audio);" in setup
    assert "noteHz[3]" in SELFTEST
    assert "1760.00f, 2217.46f, 2637.02f" in SELFTEST
    assert "for (int note = 0; note < 3; ++note)" in SELFTEST
    assert "attackMs" in SELFTEST and "releaseMs" in SELFTEST
    assert "peak = 0.98f" in SELFTEST
    assert "32767.0f" in SELFTEST
    assert "GPIO26" in SELFTEST
    assert "i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN)" in SELFTEST
    assert "i2s_set_clk(" in SELFTEST and "i2s_start(" in SELFTEST
    assert "complete 3-note chime" in SELFTEST
    assert "i2s_write(" in SELFTEST
    assert "dacWrite(" not in SELFTEST


if __name__ == "__main__":
    tests = [v for k, v in list(globals().items()) if k.startswith("test_") and callable(v)]
    for test in tests:
        test()
        print("PASS", test.__name__)
