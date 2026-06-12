# IP Camera Viewer Component for ESP32-P4

ESPHome component to display network video streams (RTSP/H264 and MJPEG) on the
ESP32-P4 with hardware decoding and LVGL display.

## 📋 Features

- ✅ **MJPEG support** - Hardware JPEG decoding optimized for network streams
- ✅ **H264/RTSP support** - Software H264 decoding (Baseline/Main Profile)
- ✅ **Hardware decoding** - ESP32-P4 hardware JPEG decoder (100 ms timeout)
- ✅ **COM marker stripping** - ffmpeg/go2rtc MJPEG compatibility
- ✅ **WiFi handling** - Automatic wait for the WiFi connection (15 s retry delay)
- ✅ **LVGL display** - Native integration with an LVGL canvas
- ✅ **RGB565** - Color format optimized for display
- ✅ **Multi-resolution** - Supports 320x240, 640x480, etc.

## 🔧 Requirements

- **Hardware:** ESP32-P4 (with hardware JPEG decoder)
- **ESPHome:** Recent version with ESP32-P4 support
- **LVGL:** Configured LVGL component
- **Network:** Configured and working WiFi

## 📦 Installation

### 1. Add the external component

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/youkorr/ip-camera-viewer
      ref: main
    components:
      - ip_camera_viewer
    refresh: 0s
```

### 2. Basic configuration

```yaml
# Network configuration
wifi:
  ssid: "YourSSID"
  password: "YourPassword"

# LVGL configuration
lvgl:
  displays:
    - display_id: my_display

# IP camera configuration
ip_camera_viewer:
  - id: security_cam_1
    url: "http://<host>:1984/api/stream.mjpeg?src=frigate1_esp32"
    protocol: mjpeg
    width: 320
    height: 240
    canvas_id: security_canvas
    update_interval: 100ms
```

## 🎬 MJPEG configuration (recommended)

### go2rtc configuration

To get an optimized MJPEG stream from your RTSP cameras:

```yaml
# go2rtc.yaml (Frigate)
go2rtc:
  streams:
    frigate1_esp32:
      - "ffmpeg:rtsp://user:pass@<host>/stream2#video=mjpeg#width=320#height=240#quality=80#fps=15"
```

### ESPHome configuration

```yaml
ip_camera_viewer:
  - id: security_cam_1
    url: "http://<host>/api/stream.mjpeg?src=frigate1_esp32"
    protocol: mjpeg
    width: 320
    height: 240
    canvas_id: security_canvas
    update_interval: 100ms
```

**Why MJPEG?**
- ✅ Hardware decoding (fast and efficient)
- ✅ Low latency
- ✅ No H264 profile issues
- ✅ COM markers automatically stripped
- ✅ Built-in JPEG validation

## 📺 LVGL integration

### Full configuration with buttons

```yaml
lvgl:
  pages:
    - id: security_page
      bg_color: 0x1a1a1a
      on_load:
        - lambda: |-
            ESP_LOGI("security", "Security page loaded - configuring canvas");

            // Configure the canvas for ip_camera_viewer
            auto canvas = id(security_canvas);
            if (canvas != nullptr) {
              lv_coord_t w = lv_obj_get_width(canvas);
              lv_coord_t h = lv_obj_get_height(canvas);
              ESP_LOGI("security", "Canvas size: %dx%d", w, h);

              if (w > 0 && h > 0) {
                // IMPORTANT: Call configure_canvas on security_cam_1
                id(security_cam_1).configure_canvas(canvas);
                ESP_LOGI("security", "Canvas configured successfully!");
              } else {
                ESP_LOGW("security", "Canvas size is 0x0, waiting for initialization");
              }
            }

      widgets:
        - canvas:
            id: security_canvas
            width: 320
            height: 240
            x: 10
            y: 10
            bg_color: 0x000000

        - label:
            id: security_title
            text: "SECURITY CAMERA"
            x: 350
            y: 10
            text_color: 0xFFFFFF

        - button:
            id: btn_start_camera
            width: 100
            height: 40
            x: 350
            y: 60
            bg_color: 0x27ae60
            on_click:
              then:
                - lambda: |-
                    ESP_LOGI("security", "Starting camera");
                    id(security_cam_1).set_enabled(true);
            widgets:
              - label:
                  text: "START"
                  text_color: 0xFFFFFF
                  align: CENTER

        - button:
            id: btn_stop_camera
            width: 100
            height: 40
            x: 350
            y: 110
            bg_color: 0xe74c3c
            on_click:
              then:
                - lambda: |-
                    ESP_LOGI("security", "Stopping camera");
                    id(security_cam_1).set_enabled(false);
            widgets:
              - label:
                  text: "STOP"
                  text_color: 0xFFFFFF
                  align: CENTER

# Global variables (optional)
globals:
  - id: cam1_state
    type: bool
    initial_value: 'false'
```

## 🔍 Troubleshooting

### Problem 1: "Canvas not configured"

**Symptom:**
```
[W][ip_camera_viewer]: Canvas not configured
```

**Solution:**
Call `configure_canvas()` in the LVGL page `on_load`:

```yaml
on_load:
  - lambda: |-
      auto canvas = id(security_canvas);
      id(security_cam_1).configure_canvas(canvas);
```

**⚠️ IMPORTANT:** Call `configure_canvas()` on `security_cam_1` (ip_camera_viewer),
NOT on `security_display` (multi_camera_display)!

### Problem 2: "WiFi not ready yet"

**Symptom:**
```
[W][ip_camera_viewer]: WiFi not ready yet, waiting for connection...
[E][ip_camera_viewer]: Host is unreachable (errno 118)
```

**Solution:**
The component automatically waits for the WiFi connection with a 15-second delay
between attempts. **No configuration required** — it is automatic!

The component checks:
1. WiFi is connected
2. STA interface is active
3. Before any camera connection attempt

### Problem 3: "COM marker data underflow"

**Symptom:**
```
[E][ip_camera_viewer]: jpeg_parse_com_marker(63): COM marker data underflow
```

**Solution:**
✅ **Already fixed!** The component automatically strips the COM markers added by
ffmpeg/go2rtc that are incompatible with the ESP32-P4 decoder.

Expected logs:
```
[I][ip_camera_viewer]: Stripping COM marker at offset 2 (length 17 bytes)
[D][ip_camera_viewer]: Stripped COM markers: 1585 -> 1568 bytes (saved 17 bytes)
```

### Problem 4: H264 "No frames decoded"

**Symptom:**
```
[W][ip_camera_viewer]: No H264 frames decoded yet (1000 attempts)
```

**Solution:**
✅ **Already fixed!** The component now sends SPS/PPS with the **first frame**
(I-frame or P-frame), not only with I-frames.

Expected logs:
```
[I][ip_camera_viewer]: Sent SPS+PPS (26+8 bytes) with FIRST frame (NAL type 1)
[I][ip_camera_viewer]: First frame decoded successfully! Decoder initialized and working.
```

### Problem 5: Canvas size 0x0

**Symptom:**
```
[W][security]: Canvas size is 0x0, waiting for initialization
```

**Solution:**
Do NOT configure the canvas in the page `on_load`, but in a button after LVGL is
initialized:

```yaml
on_click:
  then:
    - lambda: |-
        static bool canvas_configured = false;
        if (!canvas_configured) {
          auto canvas = id(security_canvas);
          lv_coord_t w = lv_obj_get_width(canvas);
          if (w > 0 && lv_obj_get_height(canvas) > 0) {
            id(security_cam_1).configure_canvas(canvas);
            canvas_configured = true;
          }
        }
        id(security_cam_1).set_enabled(true);
```

## 📊 Success logs

### Working MJPEG

```
[I][ip_camera_viewer]: WiFi ready, starting camera...
[I][ip_camera_viewer]: MJPEG connected - Status: 200
[I][ip_camera_viewer]: First JPEG frame: 1585 bytes
[I][ip_camera_viewer]: Stripping COM marker at offset 2 (length 17 bytes)
[I][ip_camera_viewer]: First JPEG frame analysis:
[I][ip_camera_viewer]:   Size: 1568 bytes
[I][ip_camera_viewer]:   SOI marker: 0xFFD8 (valid FFD8)
[I][ip_camera_viewer]:   Format: Baseline DCT (SOF0) - fully supported
[I][ip_camera_viewer]: First JPEG decoded successfully: 153600 bytes output
[I][ip_camera_viewer]: Frames: 100 - FPS: 15.0
```

### Working H264

```
[I][ip_camera_viewer]: WiFi ready, starting camera...
[I][ip_camera_viewer]: RTSP connected
[I][ip_camera_viewer]: SPS received: 26 bytes
[I][ip_camera_viewer]: PPS received: 8 bytes
[I][ip_camera_viewer]: Sent SPS+PPS (26+8 bytes) with FIRST frame (NAL type 1)
[I][ip_camera_viewer]: Frame #1: NAL type 1 (P-frame), size 2847 bytes
[I][ip_camera_viewer]: First frame decoded successfully! Decoder initialized and working.
[I][ip_camera_viewer]:   Decoded YUV size: 115200 bytes
```

## ⚙️ H264/RTSP configuration

### Tapo camera configuration

```yaml
ip_camera_viewer:
  - id: security_cam_1
    url: "rtsp://username:password@192.168.1.56:554/stream2"
    protocol: h264
    width: 320
    height: 240
    canvas_id: security_canvas
    update_interval: 100ms
```

### go2rtc configuration (H264 proxy)

```yaml
go2rtc:
  streams:
    frigate1:
      - rtsp://username:password@192.168.1.56:554/stream1
```

**⚠️ H264 limitations:**
- Supports **Baseline** and **Main Profile** only
- **High Profile** (Tapo C500 default) is not supported
- A large GOP can cause delays
- Slower than MJPEG (software decoding)

**💡 Recommendation:** Use MJPEG via go2rtc for better performance!

## 🎯 Multi-camera configuration

```yaml
ip_camera_viewer:
  - id: security_cam_1
    url: "http://192.168.1.38:1984/api/stream.mjpeg?src=cam1"
    protocol: mjpeg
    width: 320
    height: 240
    canvas_id: canvas1

  - id: security_cam_2
    url: "http://192.168.1.38:1984/api/stream.mjpeg?src=cam2"
    protocol: mjpeg
    width: 320
    height: 240
    canvas_id: canvas2
```

## 📝 Lambda API

### Available methods

```cpp
// Enable/disable the camera
id(security_cam_1).set_enabled(true);
id(security_cam_1).set_enabled(false);

// Configure the LVGL canvas
auto canvas = id(security_canvas);
id(security_cam_1).configure_canvas(canvas);

// Check the state
bool is_running = id(security_cam_1).is_enabled();
```

## 🔬 Technical details

### Applied fixes

1. **Critical H264 SPS/PPS fix**
   - Sends SPS/PPS with the FIRST frame (not only I-frames)
   - Avoids "No frames decoded" when the stream starts with P-frames
   - File: `ip_camera_viewer.cpp`

2. **MJPEG COM marker fix**
   - Strips COM markers (FF FE) added by ffmpeg
   - The ESP32-P4 hardware decoder does not support COM markers
   - Function: `strip_jpeg_com_markers_()`
   - File: `ip_camera_viewer.cpp`

3. **JPEG timeout fix**
   - Timeout increased from 40 ms -> 100 ms
   - Required for network latency
   - File: `ip_camera_viewer.cpp`

4. **WiFi timing fix**
   - Automatically waits for the WiFi connection
   - 15 s delay between attempts
   - Checks `is_connected()` and `has_sta()`
   - File: `ip_camera_viewer.cpp`

5. **WiFi API compatibility fix**
   - Replaces `get_ip_address()` with `has_sta()`
   - Compatible with newer ESPHome versions
   - File: `ip_camera_viewer.cpp`

### Data format

- **MJPEG input:** JPEG Baseline DCT (SOF0)
- **H264 input:** NAL units, Annex B format (00 00 00 01)
- **Output:** RGB565 (2 bytes/pixel)
- **Buffer:** 320x240 = 153600 bytes RGB565

### Performance

- **MJPEG:** ~15 FPS @ 320x240 (hardware decoding)
- **H264:** ~10 FPS @ 320x240 (software decoding)
- **SRAM memory:** ~220 KB
- **PSRAM memory:** ~6.7 MB

## 🐛 Debugging

### Enable verbose logs

```yaml
logger:
  level: DEBUG
  logs:
    ip_camera_viewer: DEBUG
```

### Test the stream

```bash
# Test MJPEG in a browser
http://192.168.1.38:1984/api/stream.mjpeg?src=frigate1_esp32

# Test H264 with ffplay
ffplay -rtsp_transport tcp rtsp://user:pass@192.168.1.56:554/stream2
```

## 📚 Resources

- **Repository:** https://github.com/youkorr/ip-camera-viewer
- **ESPHome:** https://esphome.io
- **go2rtc:** https://github.com/AlexxIT/go2rtc

## 📄 License

The source code of this component is original work by youkorr. Because it is an
ESPHome external component, it follows the **same dual-license arrangement as
ESPHome** so the two can be combined and distributed without any license
conflict:

- **C++/runtime code** (`.c`, `.cpp`, `.h`, `.hpp`, `.tcc`, `.ino`) is licensed
  under the **GPLv3** (it is compiled and linked against ESPHome's GPLv3 C++
  runtime).
- **Python code and all other parts** of this repository are licensed under the
  **MIT** license.

See the [LICENSE](LICENSE) file for the full text of both licenses.

### Third-party acknowledgements

- **ESPHome** (https://github.com/esphome/esphome) — Copyright (c) 2019 ESPHome,
  dual-licensed MIT/GPLv3.
- **Espressif ESP-IDF** components (ESP32-P4 hardware JPEG decoder, `esp_h264`
  decoder, etc.) — Copyright (c) Espressif Systems, licensed under their own
  terms (predominantly Apache-2.0). No Espressif source code is redistributed in
  this repository; only its public APIs are used at build time.

## 🙏 Support

For problems or questions:
1. Check the "Troubleshooting" section above
2. Enable DEBUG logs
3. Open an issue on GitHub with the full logs

---

**Version:** 1.0.0
