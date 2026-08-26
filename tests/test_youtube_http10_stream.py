from pathlib import Path
SRC = Path("src/main.cpp").read_text(encoding="utf-8")

def test_youtube_stream_forces_http10_to_avoid_chunked_body():
    assert SRC.count('ytStreamHttp.useHTTP10(true);') >= 2
    assert 'ytStreamHttp.setTimeout(10000);' in SRC

if __name__ == '__main__':
    test_youtube_stream_forces_http10_to_avoid_chunked_body()
    print('PASS')
