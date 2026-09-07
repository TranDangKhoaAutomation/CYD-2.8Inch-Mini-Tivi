from pathlib import Path

SRC = Path("src/main.cpp").read_text(encoding="utf-8")

def test_home_brand_is_simple():
    assert 'CYD MINI TV' not in SRC
    assert 'MINI TIVI' in SRC
    assert 'MINI TV' in SRC

def test_youtube_osd_stays_outside_video_rect():
    assert 'YT_VIDEO_Y = 30' in SRC
    assert 'YT_VIDEO_H = 180' in SRC
    assert 'YT_OSD_TOP_H = 28' in SRC
    assert 'YT_OSD_BOTTOM_Y = 216' in SRC
    assert 'draw16bitBeRGBBitmap(0, YT_VIDEO_Y' in SRC
    assert 'shorten(ytPlayingTitle' in SRC
    assert 'ui().text(remoteSourceLabel, 160, 228, 2);' in SRC
    assert 'remoteSourceLabel = \"YouTube\"' in SRC
    assert 'remoteSourceLabel = \"VTV Go\"' in SRC

if __name__ == '__main__':
    test_home_brand_is_simple()
    test_youtube_osd_stays_outside_video_rect()
    print('PASS')
