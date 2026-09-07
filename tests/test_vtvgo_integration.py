from pathlib import Path
import importlib.util
import sys

ROOT = Path(__file__).resolve().parents[1]
SERVER_PATH = ROOT / 'tools' / 'youtube_tv_server' / 'server.py'
MAIN = (ROOT / 'src' / 'main.cpp').read_text(encoding='utf-8')


def load_server():
    spec = importlib.util.spec_from_file_location('minitv_server_test', SERVER_PATH)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


def test_vtvgo_channel_catalog_and_routes():
    s = load_server()
    assert hasattr(s, 'VTVGO_CHANNELS')
    ids = [c['id'] for c in s.VTVGO_CHANNELS]
    assert 'vtv1' in ids and 'vtv2' in ids and 'vtv3' in ids
    routes = {r.rule for r in s.app.url_map.iter_rules()}
    assert '/api/tv/channels' in routes
    assert '/api/tv/play' in routes


def test_tv_channels_endpoint_returns_named_channels():
    s = load_server()
    c = s.app.test_client()
    r = c.get('/api/tv/channels')
    assert r.status_code == 200
    data = r.get_json()
    assert data['ok'] is True
    assert any(x['name'] == 'VTV1' for x in data['items'])


def test_vtv_resolver_uses_v21_api_and_requires_real_media_segment():
    src = SERVER_PATH.read_text(encoding="utf-8")
    assert 'https://api.vtvdigital.org/live-channel/v21.0/playback/source' in src
    assert 'VTV_PLAYBACK_API' in src
    assert 'urljoin' in src
    assert 'BANDWIDTH' in src
    assert 'iter_content' in src

    s = load_server()
    calls = []

    class FakePostResponse:
        status_code = 200
        def raise_for_status(self): pass
        def json(self):
            return {"data":{"sourceModes":[{"id":"default","isVip":0,"multiSource":[
                {"drmTypeID":"none","drmInfo":{"drmType":"none"},"manifestType":"hls","sources":[{"url":"https://dead.test/master.m3u8"}]},
                {"drmTypeID":"none","drmInfo":{"drmType":"none"},"manifestType":"hls","sources":[{"url":"https://good.test/master.m3u8"}]}
            ]}]}}

    class FakeResponse:
        def __init__(self, text='', status=200, chunks=None):
            self.status_code=status; self.text=text; self.content=text.encode(); self._chunks=chunks or [b'x']
        def raise_for_status(self):
            if self.status_code >= 400: raise s.requests.HTTPError(str(self.status_code))
        def iter_content(self, chunk_size=1024):
            yield from self._chunks
        def close(self): pass

    master = """#EXTM3U
#EXT-X-STREAM-INF:BANDWIDTH=1900000,RESOLUTION=1280x720
hi/index.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=450000,RESOLUTION=426x240
low/index.m3u8
"""
    media = """#EXTM3U
#EXT-X-TARGETDURATION:6
#EXTINF:6.0,
seg001.ts
"""


    def fake_post(url, **kwargs):
        calls.append(('post',url)); return FakePostResponse()
    def fake_get(url, **kwargs):
        calls.append(('get',url))
        if url.endswith('master.m3u8'): return FakeResponse(master)
        if 'low/index.m3u8' in url: return FakeResponse(media)
        if 'hi/index.m3u8' in url: raise AssertionError('resolver must prefer lowest bandwidth variant first')
        if 'dead.test' in url and url.endswith('seg001.ts'):
            raise s.requests.ReadTimeout('segment dead')
        if 'good.test' in url and url.endswith('seg001.ts'):
            return FakeResponse('', chunks=[b'actual-media-bytes'])
        raise AssertionError(url)

    s.requests.post=fake_post; s.requests.get=fake_get
    channel={"id":"vtv1","api_id":"1","name":"VTV1","url":"https://vtvgo.vn/channel/1"}
    chosen=s.resolve_vtvgo_source(channel)
    assert chosen == 'https://good.test/low/index.m3u8'
    assert ('get','https://dead.test/low/seg001.ts') in calls
    assert ('get','https://good.test/low/seg001.ts') in calls



def test_tv_play_uses_resolved_non_drm_hls_source():
    s = load_server()
    s.resolve_vtvgo_source = lambda channel: 'https://example.test/live/master.m3u8'
    c = s.app.test_client()
    r = c.post('/api/tv/play', json={'id': 'vtv1'})
    assert r.status_code == 200
    data = r.get_json()
    assert data['ok'] is True
    snap = s._state_snapshot()
    assert snap['source_kind'] == 'direct_hls'
    assert snap['source_url'].endswith('/master.m3u8')


def test_firmware_has_live_tv_screen_and_localized_home_button():
    assert 'TV_BROWSER' in MAIN
    assert 'TRUYỀN HÌNH' in MAIN
    assert 'LIVE TV' in MAIN
    assert '/api/tv/channels' in MAIN
    assert '/api/tv/play' in MAIN


def test_live_generators_restart_ffmpeg_instead_of_ending_http_stream():
    src = SERVER_PATH.read_text(encoding="utf-8")
    assert 'def refresh_live_source(' in src
    assert 'LIVE_RETRY_DELAY_SEC' in src
    v = src.split('def mjpeg_generator():',1)[1].split('def thumb_cache_path',1)[0]
    a = src.split('def audio_generator():',1)[1].split('@app.post("/play")',1)[0]
    assert 'while True:' in v
    assert 'refresh_live_source(' in v
    assert '[LIVE][VIDEO] restarting ffmpeg' in v
    assert 'while True:' in a
    assert 'refresh_live_source(' in a
    assert '[LIVE][AUDIO] restarting ffmpeg' in a


def test_live_mjpeg_flushes_http_headers_before_slow_prebuffer():
    src = SERVER_PATH.read_text(encoding="utf-8")
    v = src.split('def mjpeg_generator():', 1)[1].split('def thumb_cache_path', 1)[0]
    assert 'yield b"\\r\\n"' in v
    assert v.index('yield b"\\r\\n"') < v.index('while frame_queue.qsize() < prebuffer_target')


def test_firmware_gives_live_tv_more_than_ten_seconds_to_open_video():
    tv = MAIN.split('static bool startTVChannel', 1)[1].split('static void enterLiveTV', 1)[0]
    assert 'ytStreamHttp.useHTTP10(true);' in tv
    assert 'ytStreamHttp.setTimeout(30000);' in tv


def test_refresh_live_source_re_resolves_vtv_and_updates_both_urls():
    s = load_server()
    s._find_vtvgo_channel = lambda channel_id: {"id": channel_id, "name": "VTV1", "url": "https://vtvgo.test/vtv1"}
    calls = []
    def fake_resolve(channel):
        calls.append(channel["id"])
        return "https://fresh.test/master.m3u8"
    s.resolve_vtvgo_source = fake_resolve
    with s.state_lock:
        s.state["source_kind"] = "direct_hls"
        s.state["channel_id"] = "vtv1"
        s.state["source_url"] = "https://old.test/master.m3u8"
        s.state["audio_url"] = "https://old.test/master.m3u8"
    fresh = s.refresh_live_source()
    assert fresh == "https://fresh.test/master.m3u8"
    assert calls == ["vtv1"]
    snap = s._state_snapshot()
    assert snap["source_url"] == fresh
    assert snap["audio_url"] == fresh

if __name__ == '__main__':
    tests = [v for k, v in list(globals().items()) if k.startswith('test_') and callable(v)]
    for t in tests:
        t()
        print('PASS', t.__name__)
