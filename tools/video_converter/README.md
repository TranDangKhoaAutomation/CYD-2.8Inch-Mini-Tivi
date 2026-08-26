# CYD Video Converter

Tool chuyển video MP4/MKV/AVI/WebM sang định dạng đọc ổn định trên ESP32 CYD 2.8 inch.

## Chạy giao diện

Mở:

    D:\CYD-MiniTV\tools\CYD-Video-Converter.bat

Preset mặc định khuyến nghị:
- 320x240
- 12 fps
- MJPEG
- yuvj420p
- JPEG q=10
- IDX CYD1

Nếu cắm thẻ SD removable, tool ưu tiên thư mục `videos` trên thẻ làm nơi xuất.

## File tạo ra

    TEN_VIDEO.mjpeg
    TEN_VIDEO.idx

MP3 là tùy chọn. Firmware MiniTV hiện chưa dùng audio trong SD player, nên mặc định tắt để tiết kiệm dung lượng và tải xử lý.

## CLI

    python cli.py input.mp4 -o E:\videos --name TEN_VIDEO --preset stable

Preset:
- stable: 320x240 @ 12 fps q=10
- smooth: 320x240 @ 15 fps q=10
- quality: 320x240 @ 12 fps q=7

Tool tự verify từng frame JPEG và tạo `.idx` sau khi convert.