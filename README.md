# MINI TIVI – ESP32-2432S028R 2.8"

Firmware **Mini Tivi** cho board ESP32-2432S028R (CYD 2.8" resistive touch), hỗ trợ phát video từ thẻ SD và xem nội dung online qua server PC trong cùng mạng LAN.

Repository: https://github.com/TranDangKhoaAutomation/CYD-2.8Inch-Mini-Tivi

## 1. Trạng thái hiện tại

| Chức năng | Trạng thái | Ghi chú |
|---|---:|---|
| Giao diện cảm ứng 320×240 | ✅ | Arduino_GFX + ILI9341, rotation 1, display inversion bật cho panel hiện tại |
| Tiếng Việt / English | ✅ | Chọn ngay trên trang chủ, lưu vào NVS |
| Touch XPT2046 | ✅ | Hiệu chuẩn 4 điểm, lưu NVS; có lệnh `touchcal` để hiệu chuẩn lại |
| Video MJPEG từ thẻ SD | ✅ | Hỗ trợ video dài, `.idx` đọc offset theo yêu cầu để tiết kiệm RAM |
| YouTube tìm kiếm | ✅ | Tìm kiếm trên màn hình bằng bàn phím QWERTY |
| YouTube video | ✅ | PC lấy nguồn và chuyển mã sang MJPEG ~320×180 / 6 fps |
| VTV Go / Truyền hình | ✅ | PC lấy nguồn HLS không DRM từ web player VTV Go và chuyển mã cho ESP32 |
| VTV1 / VTV2 / VTV3 | ✅ đã test | Đã xác nhận server trả frame MJPEG thật |
| Audio online | 🚧 | **Chưa tích hợp vào firmware hiện tại** |
| Audio SD | 🚧 | Converter có thể tạo `.mp3`, nhưng firmware hiện tại đang tắt audio |
| Chạy online không cần PC | ❌ | YouTube/VTV Go hiện cần server PC; video SD vẫn chạy độc lập |

> **Quan trọng:** Tắt PC thì mục **YouTube** và **Truyền hình** sẽ không phát. Mục **Video thẻ SD** vẫn hoạt động độc lập trên ESP32.

---

## 2. Kiến trúc

```text
Video SD
  └── microSD -> ESP32 -> JPEGDEC -> Arduino_GFX -> ILI9341

YouTube
  └── Internet -> PC server -> yt-dlp -> FFmpeg -> MJPEG/HTTP -> ESP32

VTV Go
  └── VTV Go playback API v21 -> PC server -> HLS không DRM đã probe segment
                           -> FFmpeg -> MJPEG/MP3 qua HTTP -> ESP32
```

PC và ESP32 phải ở **cùng mạng LAN/Wi-Fi**. Firmware tự tìm server bằng UDP discovery.

- HTTP server: `8876`
- UDP discovery: `4210`
- ESP32 discovery client thường bind local port: `4211`

---

## 3. Phần cứng

### Board

- ESP32-2432S028R
- TFT ILI9341 320×240
- Touch XPT2046 resistive
- MicroSD
- CH340 USB-UART

### TFT

| Tín hiệu | GPIO |
|---|---:|
| MISO | 12 |
| MOSI | 13 |
| SCLK | 14 |
| CS | 15 |
| DC | 2 |
| Backlight | 21 |

Firmware dùng **Arduino_GFX** ở 40 MHz và gọi `invertDisplay(true)` vì panel thực tế đã được xác nhận cần display inversion để màu đúng.

### Touch XPT2046

| Tín hiệu | GPIO |
|---|---:|
| CLK | 25 |
| CS | 33 |
| DIN / MOSI | 32 |
| DO / MISO | 39 |
| IRQ | 36 |

Touch dùng software SPI và có calibration 4 điểm lưu trong NVS.

### MicroSD

| Tín hiệu | GPIO |
|---|---:|
| CS | 5 |
| SCK | 18 |
| MISO | 19 |
| MOSI | 23 |

### Audio trên board

Board có đường audio **GPIO26 -> FM8002A -> loa**. Phần cứng này sẽ được tận dụng cho audio, nhưng **bản firmware hiện tại chưa bật decoder/phát audio**.

---

## 4. Chuẩn bị môi trường trên Windows

### 4.1 Clone repository

```powershell
git clone https://github.com/TranDangKhoaAutomation/CYD-2.8Inch-Mini-Tivi.git
cd CYD-2.8Inch-Mini-Tivi
```

Nếu đã clone:

```powershell
git pull
```

### 4.2 Cài PlatformIO

Có thể dùng VS Code + PlatformIO extension hoặc PlatformIO CLI.

Kiểm tra:

```powershell
pio --version
```

Nếu `pio` không có trong PATH nhưng PlatformIO đã được cài theo user Windows, có thể gọi trực tiếp:

```powershell
$PIO = "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe"
& $PIO --version
```

### 4.3 Tìm cổng COM của ESP32

```powershell
pio device list
```

Hoặc:

```powershell
Get-CimInstance Win32_SerialPort | Select-Object DeviceID, Name
```

Máy đang phát triển project này thường nhận board là **CH340 / COM9**, nhưng COM có thể khác trên máy khác.

---

## 5. Build firmware ESP32

Tại thư mục gốc repository:

```powershell
pio run -e cyd
```

Nếu dùng đường dẫn PlatformIO đầy đủ:

```powershell
$PIO = "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe"
& $PIO run -e cyd
```

Build hiện dùng:

- `espressif32`
- `board = esp32dev`
- Arduino framework
- Arduino_GFX `v1.6.1`
- JPEGDEC `1.8.2`
- WiFiManager
- ArduinoJson
- partition `huge_app.csv`

---

## 6. Nạp firmware vào ESP32

Ví dụ board ở COM9:

```powershell
pio run -e cyd -t upload --upload-port COM9
```

Hoặc:

```powershell
$PIO = "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe"
& $PIO run -e cyd -t upload --upload-port COM9
```

Project có custom retry uploader trong:

```text
scripts/custom_upload.py
scripts/upload_cyd_retry.py
```

Upload speed hiện đặt ở **115200** để tăng độ ổn định.

### Nếu báo `Failed to connect to ESP32: No serial data received`

Board này có thể không tự kéo GPIO0 vào download mode. Thực hiện đúng thứ tự:

1. **Giữ BOOT**.
2. Bấm **RESET** một lần.
3. Thả RESET nhưng **vẫn giữ BOOT**.
4. Chạy lệnh upload.
5. Khi terminal xuất hiện:

```text
Chip is ESP32-D0WD-V3
Stub running...
```

thì **thả BOOT**.

Upload thành công sẽ có dạng:

```text
Writing ... (100 %)
Hash of data verified.
[CYD upload] Flash completed and verified.
[SUCCESS]
```

> Custom retry uploader chỉ retry quá trình kết nối. Nó không sửa được phần cứng auto-BOOT của board.

---

## 7. Serial Monitor và debug ESP32

Mở monitor:

```powershell
pio device monitor --port COM9 --baud 115200
```

Hoặc:

```powershell
$PIO = "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe"
& $PIO device monitor --port COM9 --baud 115200
```

> Trên một số board CH340, việc mở/đóng Serial Monitor có thể reset ESP32.

### Lệnh UART hữu ích

Gõ lệnh rồi Enter trong Serial Monitor:

| Lệnh | Công dụng |
|---|---|
| `help` | Hiện danh sách lệnh |
| `net` | In trạng thái Wi-Fi, IP, gateway, server |
| `scan` | Quét Wi-Fi |
| `discover` | Tìm server PC lại qua UDP |
| `ready` | Kiểm tra server + tải feed YouTube |
| `ytopen` | Mở stream MJPEG hiện có trên server để debug |
| `sdprobe` | Probe bus/thẻ SD |
| `sd` | Khởi tạo lại SD |
| `tftinfo` | In thông tin display |
| `tfttest` | Vẽ test TFT |
| `touch` | Bật log touch tạm thời |
| `touchcal` | Chạy lại hiệu chuẩn touch 4 điểm và lưu NVS |
| `heap` | In trạng thái heap |
| `fps` | In thống kê FPS |
| `playtest` | Phát `/videos/COLOR_TEST` nếu tồn tại |
| `playreal` | Phát `/videos/REAL_TEST` nếu tồn tại |
| `clearwifi` | Xóa Wi-Fi đã lưu rồi restart |
| `color A/B/C/D` | Chẩn đoán byte-order cũ; đường màu chuẩn hiện cố định BIG_ENDIAN |
| `colorauto` | Chẩn đoán màu legacy |
| `rbswap on/off` | Chẩn đoán R/B legacy; không phải fix màu chính thức |

Đường màu đã xác nhận đúng trên board này là:

```text
JPEGDEC RGB565_BIG_ENDIAN
  -> Arduino_GFX draw16bitBeRGBBitmap()
  -> ILI9341 40 MHz
  -> invertDisplay(true)
```

---

## 8. Server PC cho YouTube và VTV Go

### 8.1 Yêu cầu

PC cần:

- Windows 10/11
- Python 3.9+ khuyến nghị
- FFmpeg trong PATH
- Node.js trong PATH (yt-dlp hiện dùng `--js-runtimes node`)
- Google Chrome hoặc Microsoft Edge cài ở vị trí chuẩn
- PC và ESP32 cùng LAN/Wi-Fi

Kiểm tra:

```powershell
python --version
ffmpeg -version
node --version
```

Chrome hoặc Edge phải tồn tại ở một trong các vị trí chuẩn, ví dụ:

```text
C:\Program Files\Google\Chrome\Application\chrome.exe
C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe
```

### 8.2 Cài dependency Python

Từ thư mục gốc repository:

```powershell
python -m pip install -r tools\youtube_tv_server\requirements.txt
```

`requirements.txt` gồm Flask, yt-dlp, requests và Pillow.

Resolver VTV Go gọi playback API v21, lọc HLS không DRM rồi kiểm tra media playlist/segment thật trước khi chọn CDN; không cần trình duyệt tự động.

### 8.3 Chạy server nhanh bằng BAT

Tại thư mục gốc project, chạy:

```powershell
.\start_server.bat
```

Server này dùng chung cho cả **YouTube** và **VTV Go / Truyền hình**. Để dừng đúng server Mini TV (không ảnh hưởng Python khác), chạy:

```powershell
.\stop_server.bat
```

Hai file gốc này gọi các script tương ứng trong `tools\youtube_tv_server`.

```powershell
cd tools\youtube_tv_server
.\start_server.bat
```

BAT sẽ:

1. kiểm tra Python;
2. kiểm tra FFmpeg;
3. chạy `pip install -r requirements.txt`;
4. chạy `server.py`.

### 8.4 Chạy server thủ công

```powershell
cd tools\youtube_tv_server
python -m pip install -r requirements.txt
python server.py
```

Khi thành công sẽ thấy tương tự:

```text
CYD Mini TV server: http://0.0.0.0:8876
UDP discovery: 4210
* Running on http://127.0.0.1:8876
* Running on http://<IP-PC>:8876
```

Giữ cửa sổ server này mở trong lúc xem YouTube/VTV Go.

### 8.5 Windows Firewall

Nếu ESP32 báo `Chưa có máy chủ / No server`:

- cho phép `python.exe` qua **Private networks** trong Windows Firewall;
- bảo đảm ESP32 và PC cùng subnet;
- không bật AP isolation/client isolation trên router;
- kiểm tra TCP `8876` và UDP `4210` không bị chặn.

---

## 9. Kiểm tra server từ PC

Mở PowerShell mới trong khi `server.py` đang chạy.

### Trạng thái server

```powershell
Invoke-RestMethod http://127.0.0.1:8876/status
```

### Danh sách kênh truyền hình

```powershell
Invoke-RestMethod http://127.0.0.1:8876/api/tv/channels
```

### Test VTV1

```powershell
Invoke-RestMethod `
  -Method Post `
  -Uri http://127.0.0.1:8876/api/tv/play `
  -ContentType 'application/json' `
  -Body '{"id":"vtv1"}'
```

Kết quả mong đợi:

```json
{
  "ok": true,
  "stream": "/stream.mjpg",
  "title": "VTV1"
}
```

### Test tìm kiếm YouTube

```powershell
Invoke-RestMethod 'http://127.0.0.1:8876/api/search?q=ESP32'
```

### Kiểm tra stream MJPEG

Sau khi chọn YouTube hoặc VTV:

```text
http://127.0.0.1:8876/stream.mjpg
```

Có thể mở URL này bằng browser hoặc VLC để kiểm tra server trước khi debug ESP32.

---

## 10. Cách dùng YouTube trên Mini Tivi

1. Bật server PC.
2. Bật ESP32.
3. Kết nối ESP32 vào cùng Wi-Fi với PC.
4. Trang chủ chọn **YOUTUBE**.
5. Firmware tự UDP-discover server.
6. Chọn **Tìm kiếm / Search**.
7. Nhập từ khóa bằng bàn phím QWERTY cảm ứng.
8. Chọn video.
9. PC lấy nguồn YouTube và chuyển thành MJPEG khoảng 6 fps cho ESP32.

Nếu cần debug nhanh:

```text
discover
ready
ytopen
```

Runtime đã đo trên board hiện tại có thể đạt khoảng 4–6 fps tùy nguồn/mạng.

---

## 11. Cách dùng Truyền hình / VTV Go

1. Bật server PC.
2. Trên Home chọn **TRUYỀN HÌNH / LIVE TV**.
3. ESP32 gọi:

```text
GET /api/tv/channels
```

4. Chọn kênh.
5. ESP32 gọi:

```text
POST /api/tv/play
```

6. Server gọi playback API v21 của VTV Go và nhận các HLS candidate.
7. Server chỉ nhận nguồn **không DRM**, probe variant/segment thật và tự failover sang CDN còn sống.
8. FFmpeg chuyển nguồn đó thành MJPEG cho ESP32.

Catalog hiện cấu hình gồm:

- VTV1
- VTV2
- VTV3
- VTV4
- VTV5
- VTV6
- VTV7
- VTV8
- VTV9
- VTV10
- Vietnam Today
- ANTV
- QPVN
- Hà Nội 1
- Hà Nội 2

Đã kiểm thử runtime server với VTV1, VTV2 và VTV3.

> URL HLS VTV Go có token động. Không hard-code URL `.m3u8` vào firmware/server; resolver phải lấy lại source khi mở kênh.

---

## 12. Wi-Fi lần đầu

Firmware dùng WiFiManager/lưu credential trong ESP32.

Nếu Wi-Fi cũ sai hoặc muốn cấu hình lại, mở Serial Monitor và gửi:

```text
clearwifi
```

ESP32 sẽ xóa credential và restart.

Sau khi Wi-Fi kết nối, kiểm tra:

```text
net
discover
```

Log đúng sẽ có dạng:

```text
[DISCOVERY] RX from <IP-PC>:4210 payload='CYD_TV_SERVER|8876'
[SERVER] found by UDP: http://<IP-PC>:8876
```

---

## 13. Touch calibration

Nếu bấm lệch nút hoặc chạm YouTube nhưng nhảy sang SD, chạy:

```text
touchcal
```

Màn hình sẽ yêu cầu chạm các điểm calibration. Dữ liệu sau đó được lưu vào NVS, không cần calibration lại mỗi lần bật nguồn.

Để xem tọa độ touch tạm thời:

```text
touch
```

---

## 14. Chuẩn bị video cho thẻ SD

### Dùng converter CLI

Cần FFmpeg trong PATH.

Ví dụ:

```powershell
python tools\convert.py "D:\Video\movie.mp4" --fps 12 --quality 8
```

Chỉ định output base:

```powershell
python tools\convert.py "D:\Video\movie.mp4" `
  -o "D:\Output\movie" `
  --fps 12 `
  --width 320 `
  --height 240 `
  --quality 8
```

Không tạo audio:

```powershell
python tools\convert.py "D:\Video\movie.mp4" --fps 12 --no-audio
```

Xem toàn bộ option:

```powershell
python tools\convert.py --help
```

### Dùng converter GUI

```powershell
.\tools\CYD-Video-Converter.bat
```

Hoặc:

```powershell
python tools\video_converter\app.py
```

### File đầu ra

Converter có thể tạo:

```text
movie.mjpeg
movie.idx
movie.mp3
```

Chép vào thẻ SD:

```text
/videos/
  movie.mjpeg
  movie.idx
  movie.mp3
```

Các file phải dùng cùng basename.

> Firmware hiện tại phát hình SD từ `.mjpeg/.idx`; audio `.mp3` chưa được bật trong bản đang chạy.

---

## 15. Định dạng `.idx`

Header little-endian:

| Offset | Size | Nội dung |
|---:|---:|---|
| 0 | 4 | magic `CYD1` |
| 4 | 4 | FPS × 1000 (`uint32`) |
| 8 | 4 | số frame (`uint32`) |
| 12 | 4 × (n+1) | offset từng frame + file-size sentinel |

Firmware **không malloc toàn bộ index dài vào RAM**. Với video dài, offset được đọc từ file `.idx` theo frame cần thiết.

---

## 16. Chạy test

### Test VTV Go integration

```powershell
python tests\test_vtvgo_integration.py
```

### Một số regression test quan trọng

```powershell
python tests\test_display_stack.py
python tests\test_touch_calibration_runtime.py
python tests\test_long_sd_index.py
python tests\test_youtube_http10_stream.py
python tests\test_youtube_server_jitter_buffer.py
python tests\test_youtube_stream_and_branding.py
```

### Build lại sau khi sửa code

```powershell
pio run -e cyd
```

Không coi một thay đổi là hoàn tất chỉ vì build thành công; với phần video/network nên nạp board và kiểm tra log runtime thực tế.

---

## 17. Troubleshooting

### `No server / Chưa có máy chủ`

Kiểm tra theo thứ tự:

```powershell
Invoke-RestMethod http://127.0.0.1:8876/status
```

Sau đó trên ESP32:

```text
net
discover
```

Nếu PC hoạt động nhưng ESP32 không discover được, kiểm tra Firewall và cùng subnet.

### YouTube/VTV chạy vài giây rồi đứng

Kiểm tra UART `YTSTAT`:

```text
[YTSTAT] ... rx=... dec=... drop=... fps=... avail=... conn=... partial=...
```

Server hiện có PC-side jitter buffer và firmware dùng HTTP/1.0 cho stream để giảm tình trạng chunked/TCP stall.

### Log `framebuffer=FALLBACK-DIRECT`

ESP32 classic không có PSRAM và heap có thể bị phân mảnh. Khi không cấp phát đủ framebuffer chống tearing, firmware tự fallback sang decode/render trực tiếp. Video vẫn chạy nhưng có thể nháy/tearing nhiều hơn.

### Touch lệch

```text
touchcal
```

### Màu video sai / âm bản

Không đổi RGB/BGR ngẫu nhiên. Board đã xác nhận cần:

```text
RGB565_BIG_ENDIAN + draw16bitBeRGBBitmap() + invertDisplay(true)
```

### SD không nhận

```text
sdprobe
sd
```

Kiểm tra thẻ FAT32 và chân VSPI 5/18/19/23.

### Upload không vào bootloader

Thực hiện lại:

```text
Giữ BOOT -> RESET -> thả RESET -> vẫn giữ BOOT
```

Sau khi thấy `Stub running...` thì thả BOOT.

---

## 18. Cấu trúc project chính

```text
.
├── platformio.ini
├── src/
│   ├── main.cpp
│   ├── config.h
│   └── display/
├── scripts/
│   ├── custom_upload.py
│   └── upload_cyd_retry.py
├── tools/
│   ├── convert.py
│   ├── CYD-Video-Converter.bat
│   ├── video_converter/
│   └── youtube_tv_server/
│       ├── server.py
│       ├── start_server.bat
│       ├── requirements.txt
│       └── yt-dlp.exe
└── tests/
```

---

## 19. Quy trình phát triển khuyến nghị

Sau khi sửa firmware:

```powershell
# 1. Build
pio run -e cyd

# 2. Nạp
pio run -e cyd -t upload --upload-port COM9

# 3. Mở log
pio device monitor --port COM9 --baud 115200
```

Sau khi sửa server:

```powershell
cd tools\youtube_tv_server
python server.py
```

Kiểm tra:

```powershell
Invoke-RestMethod http://127.0.0.1:8876/status
```

Với VTV:

```powershell
Invoke-RestMethod http://127.0.0.1:8876/api/tv/channels
```

---

## 20. Việc đang phát triển tiếp

- Audio qua GPIO26 -> FM8002A cho SD/YouTube/VTV.
- Đồng bộ audio/video.
- Giảm nháy/tearing bằng framebuffer phù hợp heap của ESP32 classic.
- Clip tiêu đề YouTube theo chiều rộng pixel để không tràn vào thumbnail.
- Tăng khả năng tự reconnect stream khi mạng yếu.
- Mở rộng danh sách kênh truyền hình không DRM.

---

## License

Xem file LICENSE của repository nếu có. Các nội dung YouTube/VTV Go không được lưu kèm trong repository; server chỉ xử lý nguồn được truy cập tại thời điểm chạy.
