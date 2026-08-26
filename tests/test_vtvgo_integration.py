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


if __name__ == '__main__':
    tests = [v for k, v in list(globals().items()) if k.startswith('test_') and callable(v)]
    for t in tests:
        t()
        print('PASS', t.__name__)
