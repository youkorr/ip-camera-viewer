# IP Camera Viewer Component for ESP32-P4

ESPHome component to display network video streams (RTSP/H264 and MJPEG) on the
ESP32-P4 with hardware decoding and LVGL display.

## Features

- **MJPEG support** - Hardware JPEG decoding optimized for network streams
- **H264/RTSP support** - Software H264 decoding (Constrained Baseline profile only; see limitations)
- **Hardware decoding** - ESP32-P4 hardware JPEG decoder (100 ms timeout)
- **COM marker stripping** - ffmpeg/go2rtc MJPEG compatibility
- **WiFi handling** - Automatic wait for the WiFi connection (15 s retry delay)
- **LVGL display** - Native integration with an LVGL canvas
- **RGB565** - Color format optimized for display
- **Multi-resolution** - Supports 320x240, 640x480, etc.

## Requirements

- **Hardware:** ESP32-P4 (with hardware JPEG decoder)
- **ESPHome:** Recent version with ESP32-P4 support
- **LVGL:** Configured LVGL component
- **Network:** Configured and working WiFi

## Installation

### 1. Add the external component

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/youkorr/ip-camera-viewer
      ref: main
    components:
      - ip_camera_viewer
      - h264_hp
      - esp_h264
    refresh: 0s
```

> **Important — all three components must be listed.** `ip_camera_viewer`
> auto-loads `h264_hp` (edge264 High Profile decoder) and `esp_h264`
> (decoder headers), but ESPHome only downloads the components you list
> under `components:` — with `ip_camera_viewer` alone, the other two are
> never fetched and the configuration is rejected. Alternatively, omit the
> `components:` key entirely to fetch every component in the repository.

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

## MJPEG configuration (recommended)

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
- Hardware decoding (fast and efficient)
- Low latency
- No H264 profile issues
- COM markers automatically stripped
- Built-in JPEG validation

## LVGL integration

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

## Troubleshooting

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

**IMPORTANT:** Call `configure_canvas()` on `security_cam_1` (ip_camera_viewer),
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
**Already fixed!** The component automatically strips the COM markers added by
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
**Already fixed!** The component now sends SPS/PPS with the **first frame**
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

### Problem 6: Screen flicker / strobing on the camera canvas

**Symptom:**
The camera image visibly flickers or strobes (alternates between the current
frame and a stale/older one) while streaming, even though frames are decoding
correctly and at a stable rate. Other, static LVGL widgets on the same page do
not flicker.

**Cause:**
This is not a bug in `ip_camera_viewer` — it's a well-known LVGL interaction
with **double-buffered** display drivers (this includes `mipi_dsi` on the
ESP32-P4, and `rgb`/parallel RGB panels in general, which typically allocate
two hardware framebuffers for tear-free DMA output). By default, LVGL only
redraws the area it was told is "dirty" (here, just the canvas). With two
framebuffers alternating each refresh, redrawing only one of them means the
panel keeps swapping between "new frame" and "previous frame" every other
refresh — visible as a flicker, and it gets worse the more of the screen the
canvas covers.

**Solution:**
Add `full_refresh: true` to your `lvgl:` config. This makes LVGL redraw the
entire screen every refresh instead of just the dirty area, so both
framebuffers stay in sync:

```yaml
lvgl:
  full_refresh: true
  displays:
    - tab5_display
  pages:
    - id: main_page
      widgets:
        - canvas:
            id: cam_canvas
            width: 640
            height: 480
```

This costs a bit more CPU/DMA bandwidth per refresh (the whole screen is
redrawn instead of just the canvas), but on ESP32-P4 this is not noticeable
in practice and is the standard fix for any double-buffered display used with
LVGL — not specific to this component.

## Success logs

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

## H264/RTSP configuration

### Tapo camera configuration

```yaml
ip_camera_viewer:
  - id: security_cam_1
    url: "rtsp://username:password@192.168.1.56:554/stream2"
    protocol: rtsp   # "h264" is accepted as an alias
    width: 320
    height: 240
    canvas_id: security_canvas
    update_interval: 100ms
```

> **Protocol values:** use `mjpeg` (HTTP MJPEG) or `rtsp` (RTSP/H.264). `h264`
> is accepted as an alias for `rtsp`.

### Instant redisplay (`keep_alive`)

By default, turning the camera's switch off disconnects the stream and frees
its PSRAM (buffers, and for RTSP High profile, the edge264 decoder's several
MB of DPB). Turning it back on pays a reconnect + cold decode cost — on a
High profile RTSP stream this can be ~1-2 seconds before the first frame
shows. Fine for a dashboard you leave on, but too slow if an automation
(motion sensor, doorbell, ...) turns the camera on and needs it to appear
right away.

```yaml
ip_camera_viewer:
  - id: security_cam_1
    url: "rtsp://username:password@192.168.1.137:554/h264Preview_01_sub"
    protocol: rtsp
    width: 640
    height: 360
    canvas_id: security_canvas
    keep_alive: true
```

With `keep_alive: true`, turning the switch off only stops the on-screen
display — the RTSP/MJPEG connection and the decode task keep running in the
background, buffers and decoder stay allocated. Turning it back on just
resumes showing frames from the already-running stream: no reconnect, no
cold decode, effectively instant.

**Trade-off:** the camera connection, decoding, and the PSRAM it uses stay
active continuously, even while nothing is displayed — costs camera
bandwidth and a few MB of PSRAM permanently (more with `display_width`/
`display_height` resize, more still with multiple `keep_alive` cameras).
Leave it `false` (default) for cameras you don't need to pop up instantly.

### RTSP keepalive (`rtsp_keepalive`)

Some cameras close an RTSP control session that has been idle for a while
(≈30 s on Reolink) — the TCP socket stays open but no more RTP packets
arrive and the video freezes. To prevent this, the component sends a
`GET_PARAMETER` request on the control connection every 15 s by default.

```yaml
ip_camera_viewer:
  - id: security_cam_1
    url: "rtsp://username:password@192.168.1.56:554/stream2"
    protocol: rtsp
    width: 640
    height: 352
    canvas_id: security_canvas
    rtsp_keepalive: false   # default is true
```

Set `rtsp_keepalive: false` for cameras that hold the session open on their
own (many Tapo models do). The camera's reply to the `GET_PARAMETER` arrives
interleaved into the RTP stream and has to be re-synced out of it, so turning
the keepalive off removes that small amount of noise from the pipeline. Only
disable it if the stream stays alive without it — if the video freezes after
~30 s, the camera needs the keepalive, so leave it on.

### Display resize (`display_width` / `display_height`)

Many cameras' RTSP sub-streams are low resolution (e.g. 640x360 for a Reolink
or Tapo sub-stream), too small to fill a larger screen (Waveshare, Guition,
...). `display_width`/`display_height` let the canvas be bigger than the
decoded stream — the ESP32-P4's PPA (hardware Scale-Rotate-Mirror engine)
stretches the image to that size in the same hardware pass as the
YUV→RGB565 color conversion, so it costs virtually nothing extra:

```yaml
ip_camera_viewer:
  - id: security_cam_1
    url: "rtsp://username:password@192.168.1.56:554/h264Preview_01_sub"
    protocol: rtsp
    width: 640          # the STREAM's actual resolution
    height: 360
    display_width: 960  # optional: canvas/display resolution
    display_height: 720
    canvas_id: security_canvas
```

Notes:
- Both `display_width` and `display_height` must be given together, or
  omitted entirely (canvas defaults to `width`x`height`, no resize).
- The stretch is **exact** — no automatic aspect-ratio preservation. If
  `display_width`x`display_height` isn't the same aspect ratio as
  `width`x`height`, the image will be stretched non-uniformly. Pick
  dimensions matching your screen/stream on purpose (this is intentional:
  the right choice depends on your display, e.g. Waveshare vs Guition boards
  have different native resolutions).
- Only available for `protocol: rtsp`/`h264` (needs the PPA path). Not
  wired for MJPEG, which is already decoded at native resolution by the
  hardware JPEG decoder.
- If the PPA isn't available (init failure, or a runtime SRM error), the
  component logs a warning and falls back to displaying at the stream's
  native resolution rather than risking a corrupted canvas.

### Smoothing buffer (`buffer_frames`)

H.264 decode time is uneven: tiny P-frames decode in a few ms, but a keyframe
(IDR) is a full intra frame and takes much longer (≈170 ms on the ESP32-P4 for
a typical sub-stream). By default the component runs in **real-time mode**: it
shows the newest decoded frame the instant it's ready. Latency is minimal, but
every keyframe causes a brief hitch (the display repeats the last frame for a
tick or two while the IDR decodes, then jumps) — visible as periodic stutter,
especially on streams that push the decoder near its limit.

`buffer_frames` adds a **jitter buffer**: the decode task works a few frames
ahead into a small ring, and the display pulls from it at a steady cadence. The
buffered frames cover the keyframe decode, so the hitch disappears and motion
looks smooth.

```yaml
ip_camera_viewer:
  - id: security_cam_1
    url: "rtsp://username:password@192.168.1.56:554/stream2"
    protocol: rtsp
    width: 640
    height: 360
    canvas_id: security_canvas
    update_interval: 66ms   # match your camera's frame rate (15fps -> 66ms, 10fps -> 100ms)
    buffer_frames: 3        # 0 (default) = real-time, 2 or 3 = smoothed
```

Notes:
- **Trade-off:** `buffer_frames: N` adds ≈`N × update_interval` of latency (the
  image is that much "behind live") and allocates **N+2 extra RGB565 buffers**
  in PSRAM. At 800×600 that's ~1 MB each, so `buffer_frames: 3` ≈ 5 MB — make
  sure the board has the headroom (fine on a 32 MB Tab5, tight on smaller PSRAM).
- Use **2** to smooth a ~10 fps source, **3** for ~15 fps (the buffer must cover
  one keyframe decode ≈ 170 ms; 3 × 66 ms ≈ 200 ms).
- Set `update_interval` to match the camera's frame rate — a display cadence
  that doesn't match the source beats against it and causes judder on its own,
  buffer or not.
- Only for `protocol: rtsp`/`h264` (fed by the dedicated decode task). Ignored
  for MJPEG. If the decode task or the ring buffers can't be allocated, the
  component logs a warning and falls back to real-time mode.
- This does **not** speed up decoding — if the decoder genuinely can't keep up
  with the stream on average, lower the camera's frame rate, bitrate, or
  resolution, or lengthen its keyframe interval (GOP). The buffer only hides the
  *unevenness*, not a sustained shortfall.

### RTSP authentication

Credentials are taken from the URL (`rtsp://user:pass@host:port/path`). Both
**Basic** and **Digest** authentication are supported, and Digest is detected
automatically from the camera's `401` challenge — so cameras that only accept
Digest (Reolink, Hikvision, Dahua, many others) work without a proxy. A password
containing `@` is handled correctly.

### go2rtc configuration (H264 proxy)

```yaml
go2rtc:
  streams:
    frigate1:
      - rtsp://username:password@192.168.1.56:554/stream1
```

**H264 limitations (important):**
- The ESP32-P4 has **no hardware H264 decoder**. Decoding is done in software by
  Espressif's `esp_h264` (tinyH264), which supports the **Constrained Baseline
  profile only**.
- **Main and High profile streams cannot be decoded** — even though they play
  fine in VLC (VLC ships a full decoder). Most IP cameras (Reolink, Hikvision,
  Dahua, Tapo, ...) default to Main/High profile.
- The component logs the detected profile at startup, e.g.
  `H264 stream profile_idc=77 (Main)`, and warns when it is not Baseline.
- Software decoding is also slower than MJPEG and a large GOP adds latency.

**Recommended for non-Baseline cameras:** use **MJPEG via go2rtc** (decoded by the
ESP32-P4 hardware JPEG decoder — fast and reliable), or have go2rtc/ffmpeg
**transcode** the stream to H264 Baseline. See the go2rtc examples above.

**Recommendation:** Use MJPEG via go2rtc for better performance!

## Multi-camera configuration

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

## Switch (enable / disable the camera)

A `switch` platform is provided to turn each camera on or off from Home
Assistant or the ESPHome UI. The switch enables/disables the referenced
`ip_camera_viewer` instance and **persists its state** across reboots (restored
on boot, defaults to OFF).

```yaml
ip_camera_viewer:
  - id: security_cam_1
    url: "http://<host>/api/stream.mjpeg?src=cam1"
    protocol: mjpeg
    width: 320
    height: 240
    canvas_id: security_canvas

switch:
  - platform: ip_camera_viewer
    name: "Security Camera 1"
    camera_id: security_cam_1
```

Configuration variables:

- **name** (*Required*, string): the name of the switch.
- **camera_id** (*Required*, ID): the ID of the `ip_camera_viewer` instance to
  control.
- All other options from the [base Switch
  component](https://esphome.io/components/switch/index.html) (`id`, `icon`,
  `restore_mode`, etc.) are supported.

Turning the switch on is equivalent to calling `set_enabled(true)` on the
camera; turning it off calls `set_enabled(false)`.

## Lambda API

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

## Performance tuning (ESP32-P4, RTSP/H.264)

On-device measurements show H.264 decode on the P4 is **memory-bound, not
CPU-bound**: motion compensation and deblocking constantly read/write the
frame buffers in PSRAM, competing with the display (MIPI-DSI scanout, LVGL
rendering, PPA) for the same bandwidth. The two levers below attack exactly
that — they are worth trying before anything else.

### 1. Bigger L2 cache (sdkconfig)

The P4's L2 cache (through which ALL PSRAM traffic goes) defaults to 128 KB —
far smaller than one 640x360 video frame (~345 KB), so reference-frame reads
miss constantly. Espressif's own PSRAM-performance guidance recommends
raising it:

```yaml
esp32:
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_CACHE_L2_CACHE_256KB: y      # or CONFIG_CACHE_L2_CACHE_512KB if internal RAM allows
      CONFIG_CACHE_L2_CACHE_LINE_128B: y
      CONFIG_COMPILER_OPTIMIZATION_PERF: y
```

Trade-off: the L2 cache is carved out of the 768 KB internal L2MEM. 256 KB is
safe; 512 KB fits a whole reference luma plane (biggest win) but leaves only
~256 KB of internal RAM — check that WiFi buffers and the decode task stack
still fit (watch for allocation failures at boot).

Also make sure PSRAM runs at its maximum: `psram: { mode: hex, speed: 200MHz }`
on boards wired for hex mode (16-line). Quad/older settings halve the
bandwidth the decoder lives on.

### 2. Re-encode the stream on your server (go2rtc/Frigate)

If you already run go2rtc (as for the MJPEG option), you can hand it the
camera's High-profile stream and serve the ESP32 a cheaper one. This stacks
several wins in one place — measured on the edge264 bench, Baseline/CAVLC
alone decodes ~18% faster than High/CABAC at equal bitrate, and the GOP/fps
settings multiply that:

```yaml
# go2rtc.yaml
streams:
  tapo_esp32:
    - rtsp://user:pass@192.168.1.13:554/stream2
    - "ffmpeg:tapo_esp32#video=h264#raw=-c:v libx264 -profile:v baseline -preset ultrafast -tune zerolatency -r 10 -g 40 -b:v 600k"
```

- `-profile:v baseline`: no CABAC, no 8x8 transform — cheaper entropy decode
- `-r 10`: 10 fps gives the decoder comfortable headroom (10 smooth fps beat
  15 stuttering ones)
- `-g 40`: 4 s keyframe interval — 4x fewer expensive IDR stalls, and the
  latency catch-up recovers quickly when it does trigger
- `-b:v 600k`: bounded bitrate keeps worst-case (motion) P-frames small

Then point the ESP32 at `rtsp://<server>:8554/tapo_esp32`. The transcode
costs a slice of server CPU (negligible at 640x360@10) and the ESP32 side
needs no change at all. This is the single most effective way to get smooth
motion if your camera doesn't expose keyframe-interval/fps settings.

## Technical details

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

## Debugging

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

## Resources

- **Repository:** https://github.com/youkorr/ip-camera-viewer
- **ESPHome:** https://esphome.io
- **go2rtc:** https://github.com/AlexxIT/go2rtc

## License

The `ip_camera_viewer` component (everything under `components/ip_camera_viewer`)
was created and is maintained solely by **youkorr**. It is the author's original
work and does not reuse any ESPHome or Espressif source code. Because it is an
ESPHome external component, it follows the **same dual-license arrangement as
ESPHome** so the two can be combined and distributed without any license
conflict:

- **C++/runtime code** (`.c`, `.cpp`, `.h`, `.hpp`, `.tcc`, `.ino`) is licensed
  under the **GPLv3** (it is compiled and linked against ESPHome's GPLv3 C++
  runtime).
- **Python code and all other parts** of this repository are licensed under the
  **MIT** license.

See the [LICENSE](LICENSE) file for the full text of both licenses. This applies
only to the `ip_camera_viewer` component; bundled third-party code keeps its own
license (see below).

### Third-party acknowledgements

- **ESPHome** (https://github.com/esphome/esphome) — Copyright (c) 2019 ESPHome,
  dual-licensed MIT/GPLv3.
- **Espressif `esp_h264`** (bundled under `components/esp_h264`) — Copyright (c)
  Espressif Systems, licensed under **Apache-2.0**. It is included unmodified as
  a build dependency (software H.264 / RTSP decoding) so the project compiles out
  of the box, and keeps its own SPDX headers and `LICENSE`/`NOTICE` files. The
  ESP32-P4 hardware JPEG decoder is used through ESP-IDF's public APIs.

## Support

For problems or questions:
1. Check the "Troubleshooting" section above
2. Enable DEBUG logs
3. Open an issue on GitHub with the full logs

---

**Author:** youkorr
**Version:** 1.0.0
