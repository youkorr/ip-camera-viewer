#include "ip_camera_viewer.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/wifi/wifi_component.h"

#include <cstring>
#include <algorithm>
#include <cctype>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/ioctl.h>  // FIONREAD (mesure du backlog TCP, régulateur de latence)

// PPA (Pixel Processing Accelerator, ESP32-P4) : conversion YUV420->RGB565
// matérielle. Présent dans ESP-IDF >= 5.3 pour le P4 ; garde d'inclusion pour
// que le composant compile aussi sans le driver (repli scalaire).
#if __has_include(<driver/ppa.h>)
#include <driver/ppa.h>
#define USE_IPCV_PPA 1
#endif
#include "mbedtls/base64.h"
#include "mbedtls/md5.h"
#include "esp_task_wdt.h"
#include "esp_rom_sys.h"  // esp_rom_get_cpu_ticks_per_us (diag fréquence CPU)
#include "esp_timer.h"    // esp_timer_get_time (diag temps de décodage isolé)

namespace esphome {
namespace ip_camera_viewer {

static const char *const TAG = "ip_camera_viewer";

// Maximum buffer sizes - adaptive based on resolution
// Small resolution (640x480): 128KB JPEG buffer
// Medium resolution (1280x720): 256KB JPEG buffer
// Large resolution (1920x1080): 512KB JPEG buffer
static const size_t MAX_JPEG_SIZE = 512 * 1024;  // 512KB for JPEG (max)
static const size_t MAX_H264_SIZE = 256 * 1024;  // 256KB for H264 NAL units

void IPCameraViewer::setup() {
  ESP_LOGCONFIG(TAG, "Setting up IP Camera Viewer...");
  ESP_LOGI(TAG, "  URL: %s", this->url_.c_str());
  ESP_LOGI(TAG, "  Protocol: %s", this->protocol_ == Protocol::RTSP ? "RTSP/H264" : "MJPEG");
  ESP_LOGI(TAG, "  Resolution: %ux%u", this->width_, this->height_);
  ESP_LOGI(TAG, "  Update interval: %u ms", this->update_interval_);
  // DIAG perf: la vitesse de décodage edge264 dépend DIRECTEMENT de la fréquence
  // CPU. Si le P4 ne tourne pas à sa fréquence max (~360-400 MHz), le décodage est
  // ralenti d'autant. On log la fréquence réelle pour lever le doute (ticks/us = MHz).
  ESP_LOGI(TAG, "  CPU frequency: %u MHz (max P4 = 360-400 MHz)",
           (unsigned) esp_rom_get_cpu_ticks_per_us());

  if (!this->init_buffers_()) {
    ESP_LOGE(TAG, "Failed to allocate buffers");
    this->mark_failed();
    return;
  }

  if (this->protocol_ == Protocol::MJPEG) {
    if (!this->init_jpeg_decoder_()) {
      ESP_LOGE(TAG, "Failed to initialize JPEG decoder");
      this->mark_failed();
      return;
    }
  }
  // RTSP/H264: the decoder is created LAZILY (see decode_h264_to_yuv_), NOT here.
  // The H.264 profile is only known after the RTSP SDP is parsed. Eagerly creating
  // the tinyH264/h264bsd Baseline decoder spawns its prebuilt RISC-V worker task,
  // which faults ("Instruction address misaligned", core 1) on the ESP32-P4.
  // High-profile streams are handled by edge264 (h264_hp) and must NEVER create
  // that worker — so we defer the choice until the first frame is decoded.

  // Tâche de décodage H.264 dédiée : décode en fond pour ne PAS geler LVGL pendant
  // les ~11 s d'une I-frame (solution validée avec le dev ESPHome). Pile en RAM
  // interne 28 Ko (le décodage y a besoin de ~20 Ko ; pile PSRAM interdite car une
  // écriture flash/NVS pendant le décodage désactive le cache -> crash). Si la
  // création échoue (RAM interne insuffisante — pense à retirer micro_wake_word),
  // decode_task_handle_ reste nullptr et on décode en ligne (repli sûr).
  if (this->protocol_ == Protocol::RTSP) {
    BaseType_t ok = xTaskCreatePinnedToCore(&IPCameraViewer::decode_task_fn_, "ipcv_decode",
                                            28672, this, 1, &this->decode_task_handle_,
                                            tskNO_AFFINITY);
    if (ok != pdPASS || this->decode_task_handle_ == nullptr) {
      this->decode_task_handle_ = nullptr;
      ESP_LOGW(TAG, "Dedicated decode task NOT created (out of internal RAM?) — falling back "
                    "to inline decoding (LVGL may freeze during I-frames). Free internal RAM "
                    "(e.g. remove micro_wake_word) to enable it.");
      // The inline fallback in loop() never calls the PPA (it only runs the
      // scalar YUV->RGB565 conversion at native width_/height_) — without the
      // dedicated task, display resize never actually happens. Disable it so
      // render_width_()/height_() (used by update_canvas_) match what the
      // inline path really produces, instead of reporting a stride the
      // buffer's content doesn't have.
      if (this->resizing_()) {
        ESP_LOGW(TAG, "Disabling display resize (no dedicated decode task -> PPA unused) — "
                      "reverting to native %ux%u.", this->width_, this->height_);
        this->display_width_ = 0;
        this->display_height_ = 0;
      }
    } else {
      ESP_LOGI(TAG, "Dedicated H.264 decode task running — LVGL will no longer freeze.");
    }
  }

  ESP_LOGI(TAG, "IP Camera Viewer initialized");
}

// Tâche FreeRTOS dédiée : fetch RTP + décodage edge264 + conversion YUV->RGB565,
// EN FOND. Le loopTask ne fait plus que l'affichage (voir lvgl_timer_callback_).
// Aucun appel LVGL ici (LVGL n'est pas thread-safe). Non surveillée par le Task
// WDT : une I-frame de ~11 s ne provoque donc pas de reboot.
void IPCameraViewer::decode_task_fn_(void *arg) {
  IPCameraViewer *cam = static_cast<IPCameraViewer *>(arg);
  for (;;) {
    // Inactif tant que le flux n'est pas prêt ou que l'arrêt est demandé.
    if (!cam->decode_run_.load() || !cam->enabled_ || !cam->stream_connected_ ||
        cam->protocol_ == Protocol::MJPEG || cam->rgb565_buffer_a_ == nullptr ||
        cam->yuv_buffer_ == nullptr) {
      cam->decode_task_idle_.store(true);
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    // Se déclarer occupé PUIS re-vérifier la consigne d'arrêt (pattern Dekker) :
    // le loop() fait l'inverse (consigne à false PUIS lecture d'idle). Ainsi,
    // quand le loop() observe idle==true après avoir posé decode_run_=false, la
    // tâche ne peut plus toucher aux buffers -> libération sans use-after-free.
    cam->decode_task_idle_.store(false);
    if (!cam->decode_run_.load()) {
      cam->decode_task_idle_.store(true);
      continue;
    }
#ifdef USE_H264_HP_EDGE264
    // DÉCODER EN CONTINU, au rythme de la caméra — ne JAMAIS bloquer le décodage
    // sur l'affichage. L'ancien code attendait que le loopTask consomme la frame
    // convertie avant de décoder la suivante : dès que l'écran affichait moins
    // vite que la caméra n'émet (15 fps), l'excédent s'accumulait dans le tampon
    // TCP et côté caméra -> latence qui grossissait sans limite (~1 minute
    // observée). On ne peut pas sauter le DÉCODAGE d'une P-frame (elles se
    // référencent en chaîne), mais on peut sauter sa CONVERSION/AFFICHAGE : si
    // l'écran n'a pas encore consommé la frame précédente, la frame décodée est
    // simplement abandonnée (le DPB, lui, reste à jour). La latence reste ~0 et
    // c'est le rythme d'affichage qui régule, plus la file d'attente.
    if (cam->fetch_rtp_frame_()) {
      if (cam->decode_h264_to_yuv_() &&
          !cam->decode_frame_ready_.load(std::memory_order_acquire)) {
        // DIAG : temps de conversion YUV->RGB565 moyenné sur 64 frames. PPA
        // matériel quand la frame est au format O_UYY (voir decode_h264_to_yuv_),
        // conversion scalaire sinon.
        const int64_t _cv0 = esp_timer_get_time();
        bool converted;
        if (cam->yuv_is_ouyy_ && cam->ppa_ok_) {
          converted = cam->ppa_convert_(cam->current_decode_buffer_);
        } else {
          cam->convert_yuv420_to_rgb565_(cam->yuv_buffer_, cam->current_decode_buffer_,
                                         cam->width_, cam->height_);
          converted = true;
        }
        static int64_t cv_acc = 0;
        static uint32_t cv_n = 0;
        cv_acc += esp_timer_get_time() - _cv0;
        if (++cv_n == 64) {
          // DEBUG, not INFO: this fires every ~64 frames (a few times a minute
          // at streaming rate) and the UART logger itself costs CPU time.
          ESP_LOGD(TAG, "YUV->RGB565 conversion: %lld ms/frame (64-frame average%s)",
                   (long long) (cv_acc / 64000),
                   (cam->yuv_is_ouyy_ && cam->ppa_ok_) ? ", PPA" : ", CPU");
          cv_acc = 0;
          cv_n = 0;
        }
        // Release : la frame convertie dans current_decode_buffer_ est visible pour
        // le loopTask qui la lira après avoir vu decode_frame_ready_ == true.
        if (converted)
          cam->decode_frame_ready_.store(true, std::memory_order_release);
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(3));
    }
#else
    vTaskDelay(pdMS_TO_TICKS(50));
#endif
  }
}

void IPCameraViewer::loop() {
  // keep_alive fast path: if the stream is still connected in the background
  // (never disconnected by the OFF branch below) and its buffers are still
  // allocated, skip WiFi checks / reconnection / reallocation entirely and
  // just resume displaying — the decode task never stopped, so the next
  // frame is already fresh. This is the whole point of keep_alive: turn a
  // ~1-2 s cold reconnect+realloc (RTSP High Profile) into "recreate a timer".
  // Falls through to the normal path below if the connection actually died in
  // the background (stream_connected_ goes false on a real socket error — see
  // fetch_rtp_frame_) so a stale/dead session doesn't get stuck silently.
  if (this->enabled_ && this->lvgl_timer_ == nullptr && this->keep_alive_ &&
      this->stream_connected_ && this->rgb565_buffer_a_ != nullptr) {
    this->lvgl_timer_ = lv_timer_create(lvgl_timer_callback_, this->update_interval_, this);
    if (this->lvgl_timer_ != nullptr) {
      ESP_LOGI(TAG, "IP Camera Viewer display resumed (keep_alive: stream was already running)");
      return;
    }
    ESP_LOGE(TAG, "Failed to create LVGL timer");
  }

  // Start timer when enabled
  if (this->enabled_ && this->lvgl_timer_ == nullptr) {
    uint32_t now = millis();

    // CRITICAL: Check WiFi FIRST before any connection attempt
    auto wifi_component = wifi::global_wifi_component;
    if (wifi_component == nullptr || !wifi_component->is_connected()) {
      // Log only on first attempt or every 30 seconds to avoid spam
      static uint32_t last_wifi_log = 0;
      if (this->connection_attempts_ == 0 || (now - last_wifi_log) > 30000) {
        ESP_LOGW(TAG, "WiFi not ready yet, waiting for connection...");
        last_wifi_log = now;
      }
      this->last_connection_attempt_ = now;
      // Keep trying - don't disable camera
      return;
    }

    // Additional check: Verify WiFi has valid IP address
    if (!wifi_component->has_sta()) {
      static uint32_t last_ip_log = 0;
      if ((now - last_ip_log) > 30000) {
        ESP_LOGW(TAG, "WiFi connected but no STA interface yet, waiting...");
        last_ip_log = now;
      }
      this->last_connection_attempt_ = now;
      return;
    }

    // Check if we need to wait before attempting connection (retry delay)
    if (this->last_connection_attempt_ > 0 &&
        (now - this->last_connection_attempt_) < this->connection_retry_delay_) {
      // Still within retry delay period, skip this attempt
      return;
    }

    // WiFi is READY - proceed with connection
    ESP_LOGI(TAG, "WiFi ready, starting camera...");
    ESP_LOGI(TAG, "Starting IP Camera Viewer display...");
    this->connection_attempts_++;
    this->last_connection_attempt_ = now;

    // CRITICAL: Check if buffers need to be reallocated (after being freed when camera was disabled)
    if (this->rgb565_buffer_a_ == nullptr || this->rgb565_buffer_b_ == nullptr) {
      ESP_LOGI(TAG, "Buffers were freed, reallocating...");
      if (!this->init_buffers_()) {
        ESP_LOGE(TAG, "Failed to reallocate buffers");
        return;
      }
      // Also reinit decoder. Only JPEG is (re)created eagerly; the H264 decoder
      // is lazy (see decode_h264_to_yuv_) so a High-profile stream never spawns
      // the crashing tinyH264 worker.
      if (this->protocol_ == Protocol::MJPEG) {
        if (!this->init_jpeg_decoder_()) {
          ESP_LOGE(TAG, "Failed to reinitialize JPEG decoder");
          return;
        }
      }
    }

    bool connected = false;
    if (this->protocol_ == Protocol::MJPEG) {
      connected = this->connect_mjpeg_stream_();
    } else {
      connected = this->connect_rtsp_stream_();
    }

    if (!connected) {
      ESP_LOGE(TAG, "Failed to connect to stream (attempt %u, will retry in %u seconds)",
               this->connection_attempts_, this->connection_retry_delay_ / 1000);
      // Don't disable - will retry automatically after delay
      return;
    }

    // Connection successful! Reset retry counter
    ESP_LOGI(TAG, "Connection established after %u attempt(s)", this->connection_attempts_);
    this->connection_attempts_ = 0;
    // Ré-armer la tâche de décodage (elle a pu être arrêtée par un switch OFF) ;
    // annule aussi un éventuel arrêt différé si l'utilisateur a rebasculé vite.
    this->pending_shutdown_ = false;
    this->decode_run_.store(true);

    this->lvgl_timer_ = lv_timer_create(lvgl_timer_callback_, this->update_interval_, this);
    if (this->lvgl_timer_ == nullptr) {
      ESP_LOGE(TAG, "Failed to create LVGL timer");
    } else {
      ESP_LOGI(TAG, "IP Camera Viewer display started");
    }
  }

  // Stop timer when disabled
  if (!this->enabled_ && this->lvgl_timer_ != nullptr) {
    ESP_LOGI(TAG, "Stopping IP Camera Viewer display...");
    lv_timer_del(this->lvgl_timer_);
    this->lvgl_timer_ = nullptr;

    if (this->keep_alive_) {
      // keep_alive: only the display stops. Connection, buffers and the
      // decode task (RTSP/MJPEG fetch + decode + PPA) keep running exactly as
      // before — that's what lets the next ON skip straight to the fast path
      // above instead of paying the reconnect+realloc cost again.
      ESP_LOGI(TAG, "Display hidden (keep_alive: still decoding in the background)");
    } else if (this->decode_task_handle_ != nullptr) {
      // Une tâche de décodage tourne en fond et peut être EN TRAIN d'utiliser les
      // buffers/le socket. On ne libère pas ici (use-after-free) : on pose la
      // consigne d'arrêt et on libérera dès que la tâche est idle (bloc
      // pending_shutdown_ ci-dessous, re-tenté à chaque loop()). La tâche finit
      // sa frame en cours (au pire ~200-400 ms) puis se met en veille.
      this->decode_run_.store(false);
      this->pending_shutdown_ = true;
      ESP_LOGI(TAG, "Stop requested — waiting for the in-flight frame to finish before "
                    "freeing memory...");
    } else {
      if (this->protocol_ == Protocol::MJPEG) {
        this->disconnect_mjpeg_stream_();
      } else {
        this->disconnect_rtsp_stream_();
      }
      // CRITICAL: Free PSRAM buffers when camera is disabled to prevent memory overflow
      ESP_LOGI(TAG, "Freeing PSRAM buffers...");
      this->free_buffers_();
      ESP_LOGI(TAG, "IP Camera Viewer display stopped and buffers freed");
    }
  }

  // Arrêt différé (tâche de décodage) : dès que la tâche est réellement inactive
  // (decode_task_idle_ + decode_run_ false, voir le handshake dans le header),
  // déconnecter et LIBÉRER la mémoire — buffers RGB/YUV/H264 et surtout le DPB
  // edge264 (plusieurs Mo de PSRAM). Correction du switch OFF qui ne libérait rien.
  if (!this->enabled_ && this->pending_shutdown_) {
    if (this->decode_task_idle_.load()) {
      if (this->protocol_ == Protocol::MJPEG) {
        this->disconnect_mjpeg_stream_();
      } else {
        this->disconnect_rtsp_stream_();
      }
      this->free_buffers_();
      this->pending_shutdown_ = false;
      // Invalidate the display handshake: a frame flagged "ready" would point
      // into a buffer that was just freed; consuming it after re-enable would
      // show stale/garbage pixels (and nothing must ever touch freed memory).
      this->decode_frame_ready_.store(false, std::memory_order_release);
      ESP_LOGI(TAG, "IP Camera Viewer stopped — memory freed (free PSRAM: %u KB)",
               (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
    // sinon : on retente au prochain loop(), la tâche termine sa frame en cours
  }
}

void IPCameraViewer::check_network_quality_() {
  // Check network quality periodically
  uint32_t now = millis();
  if (now - this->last_quality_check_ < this->quality_check_interval_) {
    return;
  }
  this->last_quality_check_ = now;

  // Get WiFi RSSI as network quality indicator
  auto wifi_component = wifi::global_wifi_component;
  if (wifi_component == nullptr || !wifi_component->is_connected()) {
    return;
  }

  int32_t rssi = wifi_component->wifi_rssi();
  uint8_t old_level = this->current_quality_level_;

  // Classify network quality based on RSSI
  // Excellent (>= -50 dBm), Good (-50 to -70 dBm), Poor (< -70 dBm)
  if (rssi >= -50) {
    this->current_quality_level_ = 2;  // High quality
  } else if (rssi >= -70) {
    this->current_quality_level_ = 1;  // Medium quality
  } else {
    this->current_quality_level_ = 0;  // Low quality
  }

  // Log quality changes
  if (old_level != this->current_quality_level_) {
    const char *quality_names[] = {"LOW", "MEDIUM", "HIGH"};
    ESP_LOGI(TAG, "Network quality changed: %s -> %s (RSSI: %d dBm)",
             quality_names[old_level], quality_names[this->current_quality_level_], rssi);
    this->adapt_to_network_();
  }
}

void IPCameraViewer::adapt_to_network_() {
  // RTSP : NE PAS toucher au timer d'affichage. Cette adaptation date du MJPEG,
  // où le timer TIRE les données (moins de ticks = moins de bande passante). En
  // RTSP le flux est POUSSÉ par la caméra : ralentir l'affichage n'économise
  // rien — ça jette des frames décodées et ça bridait le FPS réel (l'utilisateur
  // configure 33 ms, l'adaptation l'écrasait vers 66-100 ms). La régulation de
  // charge H264 est assurée par le régulateur de latence du fetch.
  if (this->protocol_ == Protocol::RTSP)
    return;

  // Adapt update interval based on network quality
  // This reduces CPU load and network bandwidth on poor connections
  uint32_t old_interval = this->update_interval_;

  switch (this->current_quality_level_) {
    case 0:  // Low quality - reduce frame rate
      this->update_interval_ = 200;  // ~5 FPS
      ESP_LOGI(TAG, "Adapting to LOW network: 5 FPS");
      break;
    case 1:  // Medium quality - normal frame rate
      this->update_interval_ = 100;  // ~10 FPS
      ESP_LOGI(TAG, "Adapting to MEDIUM network: 10 FPS");
      break;
    case 2:  // High quality - maximum frame rate
      this->update_interval_ = 66;   // ~15 FPS
      ESP_LOGI(TAG, "Adapting to HIGH network: 15 FPS");
      break;
  }

  // Update LVGL timer period if active
  if (this->lvgl_timer_ != nullptr && old_interval != this->update_interval_) {
    lv_timer_set_period(this->lvgl_timer_, this->update_interval_);
    ESP_LOGI(TAG, "Timer period updated: %u ms -> %u ms", old_interval, this->update_interval_);
  }
}

void IPCameraViewer::lvgl_timer_callback_(lv_timer_t *timer) {
  IPCameraViewer *cam = static_cast<IPCameraViewer *>(lv_timer_get_user_data(timer));
  if (cam == nullptr || !cam->stream_connected_) {
    return;
  }

  // Check and adapt to network quality
  cam->check_network_quality_();

  bool frame_ready = false;

  if (cam->protocol_ == Protocol::MJPEG) {
    if (cam->fetch_jpeg_frame_()) {
      frame_ready = cam->decode_jpeg_to_rgb565_();
    }
  } else {
    // H.264 : si la tâche de décodage dédiée existe, le décodage (fetch RTP + edge264
    // + conversion) tourne EN FOND sur decode_task_fn_. Ici, sur le loopTask, on ne
    // fait QUE récupérer une frame déjà prête -> LVGL/tactile/audio ne gèlent plus,
    // même pendant les ~11 s d'une I-frame. Repli sur décodage en ligne si la tâche
    // n'a pas pu être créée (mémoire).
    if (cam->decode_task_handle_ != nullptr) {
      // acquire : si true, les écritures du buffer par la tâche sont visibles ici.
      frame_ready = cam->decode_frame_ready_.load(std::memory_order_acquire);
    } else {
      if (cam->fetch_rtp_frame_()) {
        if (cam->decode_h264_to_yuv_()) {
          cam->convert_yuv420_to_rgb565_(cam->yuv_buffer_, cam->current_decode_buffer_,
                                         cam->width_, cam->height_);
          frame_ready = true;
        }
      }
    }
  }

  // Compteur de ticks SANS nouvelle frame. Remis à zéro dès qu'une frame passe :
  // l'ancien compteur cumulatif ne se réinitialisait jamais et continuait de
  // logguer "No H264 frames decoded yet" alors que des frames s'affichaient.
  static uint32_t no_frame_count = 0;

  if (frame_ready) {
    no_frame_count = 0;
    cam->update_canvas_();
    cam->swap_buffers_();
    // Handshake avec la tâche de décodage : on a consommé la frame -> elle peut
    // préparer la suivante dans le buffer maintenant libre (post-swap).
    if (cam->decode_task_handle_ != nullptr && cam->protocol_ != Protocol::MJPEG)
      cam->decode_frame_ready_.store(false, std::memory_order_release);
    cam->frame_count_++;

    // Log FPS every 100 frames
    if (cam->frame_count_ % 100 == 0) {
      static uint32_t last_time = 0;
      uint32_t now = millis();
      if (last_time > 0) {
        float fps = 100000.0f / (now - last_time);
        ESP_LOGI(TAG, "Frames: %u - FPS: %.1f", cam->frame_count_, fps);
      }
      last_time = now;
    }
  } else {
    // Debug: log when no NEW frame has been ready for a while (counter resets
    // on every displayed frame, so this only fires on a genuine stall)
    no_frame_count++;
    if (no_frame_count == 100 || no_frame_count % 500 == 0) {
      if (cam->protocol_ == Protocol::RTSP) {
#ifdef USE_H264_HP_EDGE264
        if (cam->use_hp_decoder_) {
          // Surface l'état du décodeur edge264 au niveau WARN (logger par défaut) :
          //  - decode_errors > 0  -> NAL mal fournies / flux refusé (problème de feed)
          //  - errors == 0 & frames == 0 -> décode silencieux sans sortie (DPB / get_frame)
          //  - frames > 0 & displayed > 0 -> simple attente (décodage lent, ex. IDR)
          ESP_LOGW(TAG,
                   "No NEW H264 frame for %u ticks — displayed=%u, edge264: frames=%u, "
                   "decode_errors=%u, started=%d. Rising decode_errors -> NAL feed issue; "
                   "frames=0 -> get_frame output; otherwise just a slow decode (IDR in "
                   "progress). (VERBOSE logger for per-NAL detail)",
                   no_frame_count, cam->frame_count_, cam->hp_decoder_.frames_decoded(),
                   cam->hp_decoder_.decode_errors(), (int) cam->hp_started_);
        } else
#endif
          ESP_LOGW(TAG, "No new H264 frame for %u ticks (displayed=%u)", no_frame_count,
                   cam->frame_count_);
      } else {
        ESP_LOGW(TAG, "No new JPEG frame for %u ticks (displayed=%u)", no_frame_count,
                 cam->frame_count_);
      }
    }
  }
}

void IPCameraViewer::init_ppa_() {
#ifdef USE_IPCV_PPA
  if (this->ppa_client_ != nullptr) {
    this->ppa_ok_ = true;
    return;
  }
  ppa_client_config_t cfg = {};
  cfg.oper_type = PPA_OPERATION_SRM;
  ppa_client_handle_t client = nullptr;
  esp_err_t err = ppa_register_client(&cfg, &client);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "PPA unavailable (%s) — using software YUV->RGB565 conversion.",
             esp_err_to_name(err));
    this->ppa_ok_ = false;
    return;
  }
  this->ppa_client_ = client;
  this->ppa_ok_ = true;
  ESP_LOGI(TAG, "PPA ready: hardware YUV420->RGB565 conversion enabled.");
#else
  this->ppa_ok_ = false;
#endif
}

bool IPCameraViewer::ppa_convert_(uint8_t *dst_rgb565) {
#ifdef USE_IPCV_PPA
  ppa_srm_oper_config_t op = {};
  op.in.buffer = this->ouyy_buffer_;
  op.in.pic_w = this->width_;
  op.in.pic_h = this->height_;
  op.in.block_w = this->width_;
  op.in.block_h = this->height_;
  op.in.srm_cm = PPA_SRM_COLOR_MODE_YUV420;
  // Plein range + BT.601 : mêmes coefficients que la conversion scalaire
  // historique (r = y + 1.402v etc.) -> rendu visuel identique.
  op.in.yuv_range = PPA_COLOR_RANGE_FULL;
  op.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
  op.out.buffer = dst_rgb565;
  op.out.buffer_size = this->rgb565_buffer_size_;
  op.out.pic_w = this->render_width_();
  op.out.pic_h = this->render_height_();
  op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
  // 1.0f when no display_width_/height_ was configured (render_*() falls back
  // to width_/height_) -> byte-for-byte the previous no-resize behavior.
  // Otherwise the PPA stretches EXACTLY to render_width_/height_ in the same
  // hardware pass as the color conversion: no separate resize step, and no
  // aspect-ratio correction — the caller picked these dimensions on purpose.
  op.scale_x = (float) this->render_width_() / (float) this->width_;
  op.scale_y = (float) this->render_height_() / (float) this->height_;
  op.mode = PPA_TRANS_MODE_BLOCKING;  // le DMA travaille, la tâche dort
  esp_err_t err = ppa_do_scale_rotate_mirror((ppa_client_handle_t) this->ppa_client_, &op);
  if (err != ESP_OK) {
    // Échec (alignement, dimension...) : repli scalaire définitif, frame perdue.
    ESP_LOGW(TAG, "PPA SRM operation failed (%s) — falling back to software conversion.",
             esp_err_to_name(err));
    this->ppa_ok_ = false;
    // The scalar fallback (convert_yuv420_to_rgb565_) always writes at native
    // width_/height_ with a matching stride — it cannot target a differently
    // sized buffer. Turn resizing off from here on so render_width_()/height_()
    // (used by update_canvas_) go back to matching what the fallback actually
    // produces; without this the canvas would report the old, larger stride
    // and read the smaller native image as garbage.
    if (this->resizing_()) {
      ESP_LOGW(TAG, "Disabling display resize (PPA failed at runtime) — reverting to "
                    "native %ux%u.", this->width_, this->height_);
      this->display_width_ = 0;
      this->display_height_ = 0;
    }
    return false;
  }
  return true;
#else
  (void) dst_rgb565;
  return false;
#endif
}

bool IPCameraViewer::init_buffers_() {
  // ESP32-P4 JPEG decoder requires dimensions to be 16-byte aligned
  // Round up to nearest multiple of 16
  // RGB565/canvas buffers are sized on the RENDER resolution (render_width_/
  // height_), which equals width_/height_ unless display_width_/height_ was
  // configured (RTSP/H264 only — see set_display_size()). The MJPEG path is
  // never resized: render_width_()/height_() fall back to width_/height_ since
  // display_width_ stays 0 there (enforced in the Python config validator).
  uint32_t aligned_width = (this->render_width_() + 15) & ~15;
  uint32_t aligned_height = (this->render_height_() + 15) & ~15;

  ESP_LOGI(TAG, "Image dimensions: %ux%u (stream) -> %ux%u render -> %ux%u (16-byte aligned)",
           this->width_, this->height_, this->render_width_(), this->render_height_(),
           aligned_width, aligned_height);

  // RGB565 buffer size: aligned_width * aligned_height * 2 bytes, arrondi à 128
  // (le PPA exige des buffers de sortie alignés/dimensionnés sur la ligne de
  // cache L2 du P4 = 128 octets ; sans PPA c'est du simple slack inoffensif)
  this->rgb565_buffer_size_ = (aligned_width * aligned_height * 2 + 127) & ~(size_t) 127;

  // Allocate double buffers for RGB565 with 128-byte alignment (JPEG decoder
  // needs 64, the PPA needs L1+L2 cache-line alignment = 128 on the P4)
  this->rgb565_buffer_a_ = (uint8_t *)heap_caps_aligned_alloc(128, this->rgb565_buffer_size_,
                                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  this->rgb565_buffer_b_ = (uint8_t *)heap_caps_aligned_alloc(128, this->rgb565_buffer_size_,
                                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (this->rgb565_buffer_a_ == nullptr || this->rgb565_buffer_b_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate aligned RGB565 buffers (%u bytes each)", this->rgb565_buffer_size_);
    return false;
  }

  ESP_LOGI(TAG, "Allocated 64-byte aligned RGB565 buffers in SPIRAM: %u bytes each", this->rgb565_buffer_size_);

  this->current_display_buffer_ = this->rgb565_buffer_a_;
  this->current_decode_buffer_ = this->rgb565_buffer_b_;

  if (this->protocol_ == Protocol::MJPEG) {
    // OPTIMIZATION: Adaptive buffer sizing based on resolution (from webdavbox3 pattern)
    // 640x480 (307K pixels): 128KB buffer
    // 1280x720 (922K pixels): 256KB buffer
    // 1920x1080+ (2M+ pixels): 512KB buffer
    uint32_t pixel_count = this->width_ * this->height_;
    if (pixel_count <= 640 * 480) {
      this->jpeg_buffer_size_ = 128 * 1024;  // 128KB for small resolution
    } else if (pixel_count <= 1280 * 720) {
      this->jpeg_buffer_size_ = 256 * 1024;  // 256KB for medium resolution
    } else {
      this->jpeg_buffer_size_ = 512 * 1024;  // 512KB for large resolution
    }

    ESP_LOGI(TAG, "Adaptive JPEG buffer size for %ux%u: %u bytes",
             this->width_, this->height_, this->jpeg_buffer_size_);

    // OPTIMIZATION: PSRAM-first allocation strategy with fallback (from webdavbox3)
    bool using_psram = false;
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    if (free_psram > this->jpeg_buffer_size_) {
      this->jpeg_buffer_ = (uint8_t *)heap_caps_aligned_alloc(64, this->jpeg_buffer_size_,
                                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      using_psram = true;
    }

    // Fallback to internal RAM if PSRAM allocation failed
    if (this->jpeg_buffer_ == nullptr) {
      ESP_LOGW(TAG, "PSRAM allocation failed (free: %u bytes), trying internal RAM fallback", free_psram);
      this->jpeg_buffer_ = (uint8_t *)heap_caps_aligned_alloc(64, this->jpeg_buffer_size_,
                                                               MALLOC_CAP_8BIT);
      using_psram = false;
    }

    if (this->jpeg_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate JPEG buffer (%u bytes) in both PSRAM and internal RAM",
               this->jpeg_buffer_size_);
      return false;
    }

    ESP_LOGI(TAG, "Allocated 64-byte aligned JPEG buffer in %s: %u bytes (free PSRAM: %u bytes)",
             using_psram ? "PSRAM" : "internal RAM", this->jpeg_buffer_size_, free_psram);

    // CRITICAL: Allocate parse buffer in PSRAM to save SRAM (was 128KB static in SRAM!)
    // Parse buffer must be LARGER than JPEG buffer to handle:
    // - Incomplete JPEG from previous chunk
    // - MJPEG HTTP headers/boundaries
    // - New chunk data (16KB)
    // Camera motion generates 80-150KB JPEGs, so parse buffer needs to be 2x JPEG buffer!
    this->parse_buffer_size_ = this->jpeg_buffer_size_ * 2;  // 2x JPEG buffer (256KB for 640x480)
    this->parse_buffer_ = (uint8_t *)heap_caps_malloc(this->parse_buffer_size_,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (this->parse_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate parse buffer (%u bytes) in PSRAM", this->parse_buffer_size_);
      return false;
    }
    this->parse_buffer_len_ = 0;  // Reset parse buffer length
    ESP_LOGI(TAG, "Allocated parse buffer in PSRAM: %u bytes (2x JPEG buffer, saves SRAM!)",
             this->parse_buffer_size_);
  } else {
    // Allocate H264 and YUV buffers.
    // edge264's get_bytes() reads an unaligned 16-byte SIMD chunk and may over-read
    // up to ~16 bytes past the NAL `end` we hand to edge264_decode_NAL (the extra
    // bytes are masked out of the actual bitstream, but they ARE loaded). h264_buffer_
    // is filled only up to h264_buffer_size_, so we over-allocate by a 64-byte guard
    // and zero its tail: this honors edge264's documented over-read contract and
    // prevents a rare out-of-bounds PSRAM read when the very last NAL ends close to
    // the buffer limit. h264_buffer_size_ stays at MAX_H264_SIZE so fill logic is
    // unchanged; the guard is pure slack behind valid data.
    this->h264_buffer_size_ = MAX_H264_SIZE;
    this->h264_buffer_ = (uint8_t *)heap_caps_malloc(this->h264_buffer_size_ + 64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (this->h264_buffer_ != nullptr)
      memset(this->h264_buffer_ + this->h264_buffer_size_, 0, 64);

    // YUV420: width * height * 1.5 bytes
    this->yuv_buffer_size_ = this->width_ * this->height_ * 3 / 2;
    this->yuv_buffer_ = (uint8_t *)heap_caps_malloc(this->yuv_buffer_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (this->h264_buffer_ == nullptr || this->yuv_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate H264/YUV buffers");
      return false;
    }

    // Buffer O_UYY_E_VYY pour la conversion PPA matérielle (même taille 12 bpp,
    // aligné 128 pour le DMA). Optionnel : s'il manque, repli scalaire.
    size_t ouyy_size = ((size_t) this->width_ * this->height_ * 3 / 2 + 127) & ~(size_t) 127;
    this->ouyy_buffer_ = (uint8_t *) heap_caps_aligned_alloc(128, ouyy_size,
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    this->init_ppa_();
    if (this->ouyy_buffer_ == nullptr)
      this->ppa_ok_ = false;

    // display_width_/height_ (resize) relies entirely on the PPA doing the
    // stretch; the scalar fallback conversion always writes at native
    // width_/height_ with a matching stride and cannot safely target a
    // differently-sized buffer. If the PPA isn't available, degrade to no
    // resize rather than corrupt the canvas: the RGB565 buffers were already
    // sized for the larger render resolution above, so this just leaves
    // their tail unused (harmless) while render_width_()/height_() now fall
    // back to width_/height_ everywhere (canvas, PPA — unused here anyway).
    if (this->resizing_() && !this->ppa_ok_) {
      ESP_LOGW(TAG, "display_width_/height_ requested but the PPA is unavailable — "
                    "falling back to native %ux%u (no resize).", this->width_, this->height_);
      this->display_width_ = 0;
      this->display_height_ = 0;
    }
  }

  ESP_LOGI(TAG, "Buffers allocated successfully");
  return true;
}

void IPCameraViewer::free_buffers_() {
#ifdef USE_H264_HP_EDGE264
  // Release the edge264 High Profile decoder FIRST — it holds the largest PSRAM
  // consumer of all: several MB of DPB frame buffers. If it is left allocated
  // across a disable/enable toggle, PSRAM stays too full/fragmented to reallocate
  // the 1.2 MB of RGB565 buffers on re-enable ("Failed to allocate aligned RGB565
  // buffers"). The decoder is lazily re-created on the first decode after
  // re-enable (see decode_h264_to_yuv_), so freeing it here is safe.
  if (this->hp_started_) {
    this->hp_decoder_.end();
    this->hp_started_ = false;
  }
#endif

  // Free RGB565 buffers
  if (this->rgb565_buffer_a_ != nullptr) {
    free(this->rgb565_buffer_a_);
    this->rgb565_buffer_a_ = nullptr;
  }
  if (this->rgb565_buffer_b_ != nullptr) {
    free(this->rgb565_buffer_b_);
    this->rgb565_buffer_b_ = nullptr;
  }

  // Free JPEG buffer
  if (this->jpeg_buffer_ != nullptr) {
    free(this->jpeg_buffer_);
    this->jpeg_buffer_ = nullptr;
  }

  // Free parse buffer (CRITICAL: saves 128KB SRAM!)
  if (this->parse_buffer_ != nullptr) {
    free(this->parse_buffer_);
    this->parse_buffer_ = nullptr;
  }

  // Free H264 buffers
  if (this->h264_buffer_ != nullptr) {
    free(this->h264_buffer_);
    this->h264_buffer_ = nullptr;
  }
  if (this->yuv_buffer_ != nullptr) {
    free(this->yuv_buffer_);
    this->yuv_buffer_ = nullptr;
  }
  if (this->ouyy_buffer_ != nullptr) {
    free(this->ouyy_buffer_);
    this->ouyy_buffer_ = nullptr;
  }
#ifdef USE_IPCV_PPA
  // Client PPA : libéré ici (la tâche de décodage est garantie inactive quand
  // free_buffers_ est appelé — voir l'arrêt différé). Ré-enregistré au prochain
  // init_buffers_.
  if (this->ppa_client_ != nullptr) {
    ppa_unregister_client((ppa_client_handle_t) this->ppa_client_);
    this->ppa_client_ = nullptr;
  }
  this->ppa_ok_ = false;
  this->yuv_is_ouyy_ = false;
#endif

  // Reset buffer sizes
  this->rgb565_buffer_size_ = 0;
  this->jpeg_buffer_size_ = 0;
  this->parse_buffer_size_ = 0;
  this->parse_buffer_len_ = 0;
  this->h264_buffer_size_ = 0;
  this->yuv_buffer_size_ = 0;

  // Log freed memory
  size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  size_t free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  ESP_LOGI(TAG, "Buffers freed - Free PSRAM: %u bytes (%.2f MB), Free SRAM: %u bytes (%.2f KB)",
           free_psram, free_psram / 1024.0 / 1024.0,
           free_sram, free_sram / 1024.0);
}

bool IPCameraViewer::init_jpeg_decoder_() {
  jpeg_decode_engine_cfg_t decode_eng_cfg = {
      .intr_priority = 0,
      .timeout_ms = 200,  // 200ms timeout - increased for network streams with variable latency
  };

  esp_err_t ret = jpeg_new_decoder_engine(&decode_eng_cfg, &this->jpeg_decoder_);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create JPEG decoder: %s", esp_err_to_name(ret));
    return false;
  }

  ESP_LOGI(TAG, "JPEG hardware decoder initialized (timeout=200ms, optimized for network streams)");
  return true;
}

bool IPCameraViewer::init_h264_decoder_() {
  // esp_h264_dec_cfg_t (1.3.6) exposes only pic_type. There is no profile field:
  // esp_h264_dec_sw routes to tinyH264/h264bsd, which is Constrained Baseline only.
  // Main/High profile is handled separately by the edge264 (h264_hp) path.
  esp_h264_dec_cfg_sw_t dec_cfg = {};
  dec_cfg.pic_type = ESP_H264_RAW_FMT_I420;

  esp_h264_err_t ret = esp_h264_dec_sw_new(&dec_cfg, &this->h264_decoder_);
  if (ret != ESP_H264_ERR_OK) {
    ESP_LOGE(TAG, "Failed to create H264 decoder: %d", ret);
    return false;
  }

  ret = esp_h264_dec_open(this->h264_decoder_);
  if (ret != ESP_H264_ERR_OK) {
    ESP_LOGE(TAG, "Failed to open H264 decoder: %d", ret);
    return false;
  }

  // Truth: the bundled libopenh264.a is encoder-only; decoding goes through
  // tinyH264/h264bsd = CONSTRAINED BASELINE only. Main/High needs the edge264
  // (h264_hp) path. Don't claim Main/High here.
  ESP_LOGI(TAG, "H264 decoder initialized (tinyH264/h264bsd — Baseline profile only)");
  return true;
}

// ============================================================================
// MJPEG Methods
// ============================================================================

bool IPCameraViewer::connect_mjpeg_stream_() {
  if (this->stream_connected_) {
    return true;
  }

  // The MJPEG path uses the HTTP client, which cannot handle an rtsp:// URL
  // (the failure is the cryptic "No transport found"). Catch the mismatch early.
  if (this->url_.rfind("http", 0) != 0) {
    ESP_LOGE(TAG, "protocol is 'mjpeg' but the URL is not http(s): '%s'. For an rtsp:// URL use "
                  "protocol: rtsp (or h264); for MJPEG use an http:// URL (e.g. via go2rtc).",
             this->url_.c_str());
    return false;
  }

  esp_http_client_config_t config = {};
  config.url = this->url_.c_str();
  config.timeout_ms = 5000;
  // OPTIMIZATION: Larger receive buffer for better throughput (from webdavbox3 pattern)
  // Increased to 128KB to handle large JPEGs when camera moves/rotates
  // Static camera: ~20-40KB JPEGs, Moving camera: ~80-150KB JPEGs
  config.buffer_size = 131072;  // 128KB - handles complex scenes and camera motion
  config.buffer_size_tx = 1024;

  this->http_client_ = esp_http_client_init(&config);
  if (this->http_client_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create HTTP client");
    return false;
  }

  esp_err_t err = esp_http_client_open(this->http_client_, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open connection: %s", esp_err_to_name(err));
    esp_http_client_cleanup(this->http_client_);
    this->http_client_ = nullptr;
    return false;
  }

  int content_length = esp_http_client_fetch_headers(this->http_client_);
  int status_code = esp_http_client_get_status_code(this->http_client_);

  ESP_LOGI(TAG, "MJPEG connected - Status: %d", status_code);

  if (status_code != 200) {
    ESP_LOGE(TAG, "HTTP error: %d", status_code);
    esp_http_client_cleanup(this->http_client_);
    this->http_client_ = nullptr;
    return false;
  }

  // NOTE: Socket-level optimizations (TCP_NODELAY, SO_RCVBUF) are not available
  // because esp_http_client doesn't expose the underlying socket descriptor.
  // The esp_http_client_config_t provides timeout_ms and buffer_size options,
  // which are already configured above (timeout=5s, buffer=128KB).
  //
  // OPTIMIZATIONS APPLIED:
  // - HTTP client buffer: 128KB (handles moving camera JPEGs 80-150KB)
  // - Parse buffer: 128KB in fetch_jpeg_frame_()
  // - Read chunks: 16KB
  ESP_LOGI(TAG, "MJPEG stream connected (HTTP buffer: 128KB, JPEG buffer: %u bytes)", this->jpeg_buffer_size_);

  this->stream_connected_ = true;
  this->stream_connect_time_ = millis();  // Record connection time
  this->mjpeg_state_ = MjpegState::SEARCHING_BOUNDARY;

  ESP_LOGI(TAG, "Stream connected at %u ms (will reconnect every %u seconds)",
           this->stream_connect_time_, this->stream_reconnect_interval_ / 1000);

  return true;
}

void IPCameraViewer::disconnect_mjpeg_stream_() {
  if (this->http_client_ != nullptr) {
    esp_http_client_close(this->http_client_);
    esp_http_client_cleanup(this->http_client_);
    this->http_client_ = nullptr;
  }
  this->stream_connected_ = false;
}

bool IPCameraViewer::fetch_jpeg_frame_() {
  if (!this->stream_connected_ || this->http_client_ == nullptr) {
    return false;
  }

  // CRITICAL: Periodic stream reconnection to prevent WiFi buffer exhaustion
  // After 3 minutes of continuous streaming, reconnect to reset WiFi state
  uint32_t now = millis();
  if (now - this->stream_connect_time_ > this->stream_reconnect_interval_) {
    ESP_LOGI(TAG, "Periodic reconnect after %u seconds - resetting WiFi and decoder state",
             (now - this->stream_connect_time_) / 1000);
    this->disconnect_mjpeg_stream_();
    delay(500);  // Let WiFi fully reset
    if (this->connect_mjpeg_stream_()) {
      ESP_LOGI(TAG, "Stream reconnected successfully");
    } else {
      ESP_LOGW(TAG, "Stream reconnection failed - will retry");
      return false;
    }
  }

  // OPTIMIZATION: Use larger buffers for better throughput and prevent JPEG truncation
  // CRITICAL: parse_buffer must be large enough to hold complete JPEG + MJPEG overhead
  // When camera moves/rotates: JPEGs can be 80-150KB (high complexity)
  // When camera static: JPEGs are 20-40KB (low complexity)
  static const size_t CHUNK_SIZE = 16 * 1024;  // 16KB chunks for reading

  // CRITICAL: Use static temp_buffer to avoid stack overflow on loopTask
  // parse_buffer is now a class member allocated in PSRAM (saves 128KB SRAM!)
  static uint8_t temp_buffer[CHUNK_SIZE];      // 16KB temp buffer (static, OK in SRAM)
  static uint32_t total_bytes_read = 0;        // Track total for periodic yielding

  // Safety check: ensure parse buffer is allocated
  if (this->parse_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Parse buffer not allocated!");
    return false;
  }

  int read_len = esp_http_client_read(this->http_client_, (char *)temp_buffer, sizeof(temp_buffer));
  if (read_len < 0) {
    ESP_LOGE(TAG, "Stream read error");
    this->disconnect_mjpeg_stream_();
    return false;
  }
  if (read_len == 0) {
    return false;
  }

  // OPTIMIZATION: Periodic CPU yielding during large transfers (from webdavbox3)
  // Yield every 64KB to prevent watchdog timeout and allow other tasks to run
  total_bytes_read += read_len;
  if (total_bytes_read >= 64 * 1024) {
    taskYIELD();
    total_bytes_read = 0;
  }

  // Append to parse buffer
  if (this->parse_buffer_len_ + read_len > this->parse_buffer_size_) {
    // CRITICAL: Buffer overflow - discard corrupted data and reset state machine
    // Keeping partial buffer would create truncated JPEG -> DMA2D crash!
    // Instead, clear everything and search for next complete JPEG frame
    static uint32_t overflow_count = 0;
    if (overflow_count++ < 5) {
      ESP_LOGW(TAG, "Parse buffer overflow (%u + %d > %u) - discarding corrupted frame, resetting to search for next JPEG",
               this->parse_buffer_len_, read_len, this->parse_buffer_size_);
    }

    // CRITICAL: Reset MJPEG state machine to search for new frame
    this->parse_buffer_len_ = 0;         // Clear buffer
    this->mjpeg_state_ = MjpegState::SEARCHING_BOUNDARY;  // Search for FFD8
    this->jpeg_data_len_ = 0;            // Reset JPEG length
  }
  memcpy(this->parse_buffer_ + this->parse_buffer_len_, temp_buffer, read_len);
  this->parse_buffer_len_ += read_len;

  // Parse MJPEG stream - look for JPEG markers
  size_t i = 0;
  while (i < this->parse_buffer_len_ - 1) {
    if (this->mjpeg_state_ == MjpegState::SEARCHING_BOUNDARY) {
      // Look for JPEG start marker (FFD8)
      if (this->parse_buffer_[i] == 0xFF && this->parse_buffer_[i + 1] == 0xD8) {
        this->jpeg_data_len_ = 0;
        this->mjpeg_state_ = MjpegState::READING_CONTENT;
        this->jpeg_buffer_[this->jpeg_data_len_++] = 0xFF;
        this->jpeg_buffer_[this->jpeg_data_len_++] = 0xD8;
        i += 2;
        continue;
      }
      i++;
    } else if (this->mjpeg_state_ == MjpegState::READING_CONTENT) {
      // Copy data and look for end marker (FFD9)
      if (this->parse_buffer_[i] == 0xFF && this->parse_buffer_[i + 1] == 0xD9) {
        this->jpeg_buffer_[this->jpeg_data_len_++] = 0xFF;
        this->jpeg_buffer_[this->jpeg_data_len_++] = 0xD9;

        size_t remaining = this->parse_buffer_len_ - i - 2;
        if (remaining > 0) {
          memmove(this->parse_buffer_, this->parse_buffer_ + i + 2, remaining);
        }
        this->parse_buffer_len_ = remaining;

        this->mjpeg_state_ = MjpegState::SEARCHING_BOUNDARY;

        if (this->first_update_) {
          ESP_LOGI(TAG, "First JPEG frame: %u bytes", this->jpeg_data_len_);
          this->first_update_ = false;
        }

        return true;
      }

      if (this->jpeg_data_len_ < this->jpeg_buffer_size_) {
        this->jpeg_buffer_[this->jpeg_data_len_++] = this->parse_buffer_[i];
      } else {
        ESP_LOGW(TAG, "JPEG buffer overflow");
        this->mjpeg_state_ = MjpegState::SEARCHING_BOUNDARY;
        this->jpeg_data_len_ = 0;
      }
      i++;
    }
  }

  if (i < this->parse_buffer_len_ && this->mjpeg_state_ == MjpegState::SEARCHING_BOUNDARY) {
    size_t remaining = this->parse_buffer_len_ - i;
    memmove(this->parse_buffer_, this->parse_buffer_ + i, remaining);
    this->parse_buffer_len_ = remaining;
  }

  return false;
}

// Strip ALL unsupported markers from JPEG for ESP32-P4 hardware decoder
// ESP32-P4 JPEG hardware decoder only supports these markers:
// - SOI (FF D8), DQT (FF DB), DHT (FF C4), SOF0 (FF C0), SOS (FF DA), EOI (FF D9)
// - Scan data (between SOS and EOI)
// ALL other markers must be removed: APP0-15, COM, DRI, RST, etc.
size_t IPCameraViewer::strip_jpeg_com_markers_(uint8_t *data, size_t len) {
  if (len < 4) return len;

  // DEBUG: Disabled to reduce log verbosity
  // Enable by changing debug_markers = true for troubleshooting
  static uint32_t jpeg_count = 0;
  jpeg_count++;
  bool debug_markers = false;  // Set to true to enable marker debugging

  size_t write_pos = 0;
  size_t read_pos = 0;

  // Keep SOI
  if (data[0] == 0xFF && data[1] == 0xD8) {
    data[write_pos++] = 0xFF;
    data[write_pos++] = 0xD8;
    read_pos = 2;
    if (debug_markers) ESP_LOGI(TAG, "  [KEEP] SOI (FF D8)");
  }

  bool in_scan_data = false;
  bool found_sos = false;
  uint16_t sof_width = 0, sof_height = 0;

  while (read_pos < len - 1) {
    if (data[read_pos] == 0xFF) {
      uint8_t marker = data[read_pos + 1];

      // Check for byte stuffing (FF 00) - always handle this first
      if (marker == 0x00) {
        if (in_scan_data) {
          data[write_pos++] = 0xFF;
          data[write_pos++] = 0x00;
        }
        read_pos += 2;
        continue;
      }

      // EOI marker - end of scan data and JPEG
      if (marker == 0xD9) {
        in_scan_data = false;  // Exit scan data mode
        if (debug_markers) ESP_LOGI(TAG, "  [KEEP] EOI (FF D9)");
        data[write_pos++] = 0xFF;
        data[write_pos++] = 0xD9;
        break;  // End of JPEG
      }

      // CRITICAL: When in scan data, ONLY handle EOI and byte stuffing
      // All other bytes (including FF D8) are compressed image data, not markers
      if (in_scan_data) {
        data[write_pos++] = data[read_pos++];
        continue;
      }

      // CRITICAL: Detect second SOI - means concatenated JPEGs from FFmpeg
      // Only check this OUTSIDE scan data (before SOS or after EOI)
      if (marker == 0xD8 && read_pos > 2) {
        if (debug_markers) {
          ESP_LOGW(TAG, "  CONCATENATED JPEG detected at offset %u - truncating here", read_pos);
          ESP_LOGW(TAG, "  FFmpeg is sending multiple JPEGs glued together!");
        }
        // Add EOI marker to close first JPEG properly
        data[write_pos++] = 0xFF;
        data[write_pos++] = 0xD9;
        if (debug_markers) ESP_LOGI(TAG, "  [ADDED] EOI (FF D9) to close first JPEG");
        break;  // Stop processing - only use first JPEG
      }

      // SOS marker - start of scan data
      if (marker == 0xDA) {
        in_scan_data = true;
        found_sos = true;
        if (read_pos + 3 >= len) break;
        uint16_t marker_len = (data[read_pos + 2] << 8) | data[read_pos + 3];
        size_t total_len = 2 + marker_len;
        if (read_pos + total_len > len) break;

        if (debug_markers) ESP_LOGI(TAG, "  [KEEP] SOS (FF DA) - %u bytes", total_len);
        memcpy(data + write_pos, data + read_pos, total_len);
        write_pos += total_len;
        read_pos += total_len;
        continue;
      }

      // RST markers - REMOVE
      if (marker >= 0xD0 && marker <= 0xD7) {
        if (debug_markers) ESP_LOGI(TAG, "  [REMOVE] RST%d (FF %02X)", marker - 0xD0, marker);
        read_pos += 2;
        continue;
      }

      // SOF0 ONLY - ESP32-P4 hardware decoder only supports baseline JPEG, NOT progressive!
      if (marker == 0xC0) {
        if (read_pos + 3 >= len) break;
        uint16_t marker_len = (data[read_pos + 2] << 8) | data[read_pos + 3];
        size_t total_len = 2 + marker_len;

        // CRITICAL: Validate SOF0 marker is not truncated
        if (read_pos + total_len > len) {
          if (debug_markers) {
            ESP_LOGW(TAG, "  TRUNCATED SOF0 marker at offset %u (needs %u bytes, only %u available)",
                     read_pos, total_len, len - read_pos);
          }
          // JPEG is corrupted - reject entire frame
          return 0;
        }

        // Extract resolution from SOF (offset +5 for height, +7 for width)
        if (read_pos + 9 < len) {
          sof_height = (data[read_pos + 5] << 8) | data[read_pos + 6];
          sof_width = (data[read_pos + 7] << 8) | data[read_pos + 8];
        }

        if (debug_markers) {
          const char *sof_name = (marker == 0xC0) ? "SOF0 (Baseline)" : "SOF2 (Progressive)";

          // CRITICAL: Log sampling factors to diagnose decoder rejection
          // SOF format: FF C0/C2 [length] [precision] [height] [width] [num_components] [component_data...]
          uint8_t num_components = (read_pos + 9 < len) ? data[read_pos + 9] : 0;

          ESP_LOGI(TAG, "  [KEEP] %s (FF %02X) - %ux%u - %u components - %u bytes",
                   sof_name, marker, sof_width, sof_height, num_components, total_len);

          // Log sampling factors for each component
          size_t component_offset = 10;
          for (uint8_t i = 0; i < num_components && read_pos + component_offset + 2 < len; i++) {
            uint8_t component_id = data[read_pos + component_offset];
            uint8_t sampling = data[read_pos + component_offset + 1];
            uint8_t h_factor = (sampling >> 4) & 0x0F;
            uint8_t v_factor = sampling & 0x0F;
            uint8_t quant_table = data[read_pos + component_offset + 2];

            ESP_LOGI(TAG, "    Component %u: H=%u V=%u (sampling %ux%u) QT=%u",
                     component_id, h_factor, v_factor, h_factor, v_factor, quant_table);
            component_offset += 3;
          }

          // Determine chroma subsampling format
          if (num_components == 3 && read_pos + 11 + 1 < len) {
            uint8_t y_sampling = data[read_pos + 11];
            uint8_t y_h = (y_sampling >> 4) & 0x0F;
            uint8_t y_v = y_sampling & 0x0F;

            if (y_h == 2 && y_v == 2) {
              ESP_LOGI(TAG, "    Chroma subsampling: 4:2:0 (YUV420) Standard");
            } else if (y_h == 2 && y_v == 1) {
              ESP_LOGW(TAG, "    Chroma subsampling: 4:2:2 (YUV422) May not be supported");
            } else if (y_h == 1 && y_v == 1) {
              ESP_LOGW(TAG, "    Chroma subsampling: 4:4:4 (YUV444) May not be supported");
            } else {
              ESP_LOGW(TAG, "    Non-standard chroma subsampling: %ux%u", y_h, y_v);
            }
          }
        }

        memcpy(data + write_pos, data + read_pos, total_len);
        write_pos += total_len;
        read_pos += total_len;
        continue;
      }

      // DQT, DHT - KEEP (with strict validation)
      if (marker == 0xDB || marker == 0xC4) {
        if (read_pos + 3 >= len) break;
        uint16_t marker_len = (data[read_pos + 2] << 8) | data[read_pos + 3];
        size_t total_len = 2 + marker_len;

        // CRITICAL: Validate marker is not truncated
        if (read_pos + total_len > len) {
          if (debug_markers) {
            ESP_LOGW(TAG, "  TRUNCATED %s marker at offset %u (needs %u bytes, only %u available)",
                     marker == 0xDB ? "DQT" : "DHT", read_pos, total_len, len - read_pos);
          }
          // JPEG is corrupted - reject entire frame
          return 0;  // Return 0 to indicate invalid JPEG
        }

        const char *marker_name = (marker == 0xDB) ? "DQT" : "DHT";
        if (debug_markers) ESP_LOGI(TAG, "  [KEEP] %s (FF %02X) - %u bytes", marker_name, marker, total_len);

        memcpy(data + write_pos, data + read_pos, total_len);
        write_pos += total_len;
        read_pos += total_len;
        continue;
      }

      // ALL other markers - REMOVE (including SOF2 progressive JPEG)
      if ((marker >= 0xE0 && marker <= 0xEF) ||  // APP
          marker == 0xFE ||                        // COM
          marker == 0xDD ||                        // DRI
          marker == 0xDC ||                        // DNL
          (marker >= 0xC0 && marker <= 0xCF && marker != 0xC0 && marker != 0xC4)) {  // Remove SOF1-15 except SOF0 and DHT
        if (read_pos + 3 >= len) break;
        uint16_t marker_len = (data[read_pos + 2] << 8) | data[read_pos + 3];

        if (debug_markers) {
          const char *marker_name =
            (marker >= 0xE0 && marker <= 0xEF) ? "APP" :
            (marker == 0xFE) ? "COM" :
            (marker == 0xDD) ? "DRI" :
            (marker == 0xDC) ? "DNL" : "OTHER_SOF";
          ESP_LOGI(TAG, "  [REMOVE] %s (FF %02X) - %u bytes", marker_name, marker, marker_len);
        }

        read_pos += 2 + marker_len;
        continue;
      }

      // Unknown marker
      if (debug_markers) ESP_LOGW(TAG, "  [SKIP] Unknown (FF %02X)", marker);
      read_pos += 2;
    } else {
      data[write_pos++] = data[read_pos++];
    }
  }

  // Log resolution mismatch warning
  if (debug_markers && (sof_width != 0 || sof_height != 0)) {
    ESP_LOGI(TAG, "  JPEG resolution: %ux%u (configured: 640x480)", sof_width, sof_height);
    if (sof_width != 640 || sof_height != 480) {
      ESP_LOGW(TAG, "  RESOLUTION MISMATCH! Decoder expects 640x480");
    }
  }

  return write_pos;
}

size_t IPCameraViewer::strip_jpeg_restart_markers_(uint8_t *data, size_t len) {
  // This function is now integrated into strip_jpeg_com_markers_
  // Keep for compatibility but it does nothing
  return len;
}

bool IPCameraViewer::decode_jpeg_to_rgb565_() {
  if (this->jpeg_data_len_ == 0 || this->jpeg_decoder_ == nullptr) {
    return false;
  }

  // Validate JPEG markers before decoding
  if (this->jpeg_data_len_ < 4 ||
      this->jpeg_buffer_[0] != 0xFF || this->jpeg_buffer_[1] != 0xD8) {
    return false;  // Silent fail - invalid JPEG
  }

  // DEBUG: Disabled to reduce log verbosity
  // Enable by setting debug_enabled = true for troubleshooting
  static const bool debug_enabled = false;
  static uint32_t debug_count = 0;
  size_t original_len = this->jpeg_data_len_;
  bool debug_this_frame = debug_enabled && (debug_count++ < 3);

  if (debug_this_frame) {
    ESP_LOGI(TAG, "=== JPEG Debug ===");
    ESP_LOGI(TAG, "Size: %u bytes", original_len);
  }

  // CRITICAL FIX: Truncate at first EOI (FF D9) BEFORE stripping markers
  // go2rtc concatenates multiple JPEGs - we need only the first one
  bool eoi_found = false;
  for (size_t i = 0; i < this->jpeg_data_len_ - 1; i++) {
    if (this->jpeg_buffer_[i] == 0xFF) {
      uint8_t next = this->jpeg_buffer_[i + 1];
      if (next == 0xD9) {  // EOI marker found
        eoi_found = true;
        this->jpeg_data_len_ = i + 2;  // Truncate here
        break;
      } else if (next == 0x00) {
        i++;  // Skip byte stuffing (FF 00)
      }
    }
  }

  // Strip ALL unsupported markers (APP, COM, DRI, RST, etc.)
  // This is the CRITICAL FIX for ESP32-P4 hardware decoder
  size_t cleaned_len = this->strip_jpeg_com_markers_(this->jpeg_buffer_, this->jpeg_data_len_);

  // Check if marker stripping detected corrupted JPEG (returns 0)
  if (cleaned_len == 0) {
    static uint32_t truncation_errors = 0;
    if (truncation_errors++ < 3) {
      ESP_LOGW(TAG, "JPEG rejected: truncated marker detected (error #%u)", truncation_errors);
    }
    return false;  // Skip corrupted JPEG
  }

  this->jpeg_data_len_ = cleaned_len;

  // Validate we still have a valid JPEG after cleanup (silent)
  if (this->jpeg_data_len_ < 4 ||
      this->jpeg_buffer_[0] != 0xFF || this->jpeg_buffer_[1] != 0xD8 ||
      this->jpeg_buffer_[this->jpeg_data_len_ - 2] != 0xFF ||
      this->jpeg_buffer_[this->jpeg_data_len_ - 1] != 0xD9) {
    return false;  // Corrupted JPEG - skip silently
  }

  jpeg_decode_cfg_t decode_cfg = {
      .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
      .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
  };

  // Error tracking (file-scope statics for persistence)
  static uint32_t decode_errors = 0;
  static uint32_t consecutive_errors = 0;

  uint32_t out_size = 0;
  esp_err_t ret = jpeg_decoder_process(this->jpeg_decoder_, &decode_cfg,
                                       this->jpeg_buffer_, this->jpeg_data_len_,
                                       this->current_decode_buffer_, this->rgb565_buffer_size_,
                                       &out_size);

  if (ret != ESP_OK) {
    // Silently handle decode errors - FFmpeg generates optimized Huffman tables
    // that ESP32-P4 hardware decoder doesn't support. Some frames will fail,
    // but successful frames will display normally.
    decode_errors++;
    consecutive_errors++;

    // Only log first 3 errors, then stay silent
    if (decode_errors <= 3) {
      ESP_LOGW(TAG, "JPEG decode failed (error #%u): %s - will retry with next frame",
               decode_errors, esp_err_to_name(ret));
    }

    // CRITICAL: Prevent WiFi buffer exhaustion and watchdog timeout
    // After 10 consecutive errors, pause to let WiFi drain buffers
    if (consecutive_errors == 10) {
      ESP_LOGD(TAG, "10 consecutive errors - pausing 200ms to let WiFi recover");
      delay(200);  // Let WiFi drain buffers
    }

    // After 20 consecutive errors, longer pause for system recovery
    if (consecutive_errors >= 20) {
      ESP_LOGD(TAG, "20+ consecutive errors - pausing 500ms for full recovery");
      delay(500);  // Longer pause for WiFi and watchdog
      consecutive_errors = 0;  // Reset counter
    }

    return false;
  }

  // Reset consecutive error counter on successful decode
  consecutive_errors = 0;

  // Log first successful decode only
  static bool first_success = false;
  if (!first_success) {
    ESP_LOGI(TAG, "JPEG decoder working! %ux%u resolution", this->width_, this->height_);
    ESP_LOGI(TAG, "First frame: %u bytes JPEG -> %u bytes RGB565", this->jpeg_data_len_, out_size);
    first_success = true;
  }

  return true;
}

// ============================================================================
// RTSP Methods
// ============================================================================

bool IPCameraViewer::connect_rtsp_stream_() {
  if (this->stream_connected_) {
    return true;
  }

  // Parse RTSP URL: rtsp://[user:pass@]host:port/path
  std::string url = this->url_;
  if (url.find("rtsp://") != 0) {
    ESP_LOGE(TAG, "Invalid RTSP URL");
    return false;
  }

  url = url.substr(7);  // Remove "rtsp://"

  // Check for credentials (user:pass@). The credentials live in the userinfo
  // section, before the first '/', and the password itself may contain '@', so
  // split on the LAST '@' within the authority (not the first one).
  std::string credentials;
  size_t authority_end = url.find('/');
  std::string authority = (authority_end == std::string::npos) ? url : url.substr(0, authority_end);
  size_t at_pos = authority.rfind('@');
  // Reset any previous auth state (important on reconnect: the nonce is stale)
  this->rtsp_auth_.clear();
  this->rtsp_user_.clear();
  this->rtsp_pass_.clear();
  this->digest_realm_.clear();
  this->digest_nonce_.clear();
  this->digest_qop_.clear();
  this->digest_opaque_.clear();
  this->digest_nc_ = 0;
  if (at_pos != std::string::npos) {
    credentials = url.substr(0, at_pos);
    url = url.substr(at_pos + 1);  // Remove credentials from URL

    // Split user:pass (used for Digest auth). The password may contain ':'? No -
    // the first ':' separates user from password per RFC userinfo.
    size_t colon = credentials.find(':');
    if (colon != std::string::npos) {
      this->rtsp_user_ = credentials.substr(0, colon);
      this->rtsp_pass_ = credentials.substr(colon + 1);
    } else {
      this->rtsp_user_ = credentials;
    }

    // Encode credentials to Base64 for Basic auth (fallback)
    size_t out_len = 0;
    unsigned char base64_buf[256];
    if (mbedtls_base64_encode(base64_buf, sizeof(base64_buf), &out_len,
                              (const unsigned char *)credentials.c_str(), credentials.length()) == 0) {
      this->rtsp_auth_ = std::string((char *)base64_buf, out_len);
      ESP_LOGI(TAG, "RTSP credentials parsed (Basic + Digest supported)");
    }
  }

  // Now parse host:port/path
  size_t path_pos = url.find('/');
  size_t port_pos = url.find(':');

  std::string host;
  uint16_t port = 554;
  std::string path = "/";

  if (port_pos != std::string::npos && (path_pos == std::string::npos || port_pos < path_pos)) {
    host = url.substr(0, port_pos);
    if (path_pos != std::string::npos) {
      port = atoi(url.substr(port_pos + 1, path_pos - port_pos - 1).c_str());
    } else {
      port = atoi(url.substr(port_pos + 1).c_str());
    }
  } else if (path_pos != std::string::npos) {
    host = url.substr(0, path_pos);
  } else {
    host = url;
  }

  if (path_pos != std::string::npos) {
    path = url.substr(path_pos);
  }

  ESP_LOGI(TAG, "Connecting to RTSP: %s:%u%s", host.c_str(), port, path.c_str());

  // Create TCP socket for RTSP
  this->rtsp_socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (this->rtsp_socket_ < 0) {
    ESP_LOGE(TAG, "Failed to create socket");
    return false;
  }

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);

  struct hostent *he = gethostbyname(host.c_str());
  if (he == nullptr) {
    ESP_LOGE(TAG, "DNS resolution failed for %s", host.c_str());
    close(this->rtsp_socket_);
    this->rtsp_socket_ = -1;
    return false;
  }
  memcpy(&server_addr.sin_addr, he->h_addr, he->h_length);

  ESP_LOGI(TAG, "Attempting TCP connection to %s:%u...", host.c_str(), port);

  // Set socket to non-blocking for connect
  int flags = fcntl(this->rtsp_socket_, F_GETFL, 0);
  fcntl(this->rtsp_socket_, F_SETFL, flags | O_NONBLOCK);

  // Start non-blocking connect
  int ret = connect(this->rtsp_socket_, (struct sockaddr *)&server_addr, sizeof(server_addr));
  if (ret < 0 && errno != EINPROGRESS) {
    ESP_LOGE(TAG, "Failed to start connect: %s (errno %d)", strerror(errno), errno);
    close(this->rtsp_socket_);
    this->rtsp_socket_ = -1;
    return false;
  }

  // Wait for connection with timeout, feeding watchdog periodically
  bool connected = false;
  for (int i = 0; i < 10; i++) {  // 10 x 500ms = 5 seconds max
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(this->rtsp_socket_, &write_fds);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000;  // 500ms

    int sel_ret = ::select(this->rtsp_socket_ + 1, nullptr, &write_fds, nullptr, &tv);

    if (sel_ret > 0) {
      // Check if connection succeeded
      int so_error;
      socklen_t len = sizeof(so_error);
      getsockopt(this->rtsp_socket_, SOL_SOCKET, SO_ERROR, &so_error, &len);

      if (so_error == 0) {
        connected = true;
        break;
      } else {
        ESP_LOGE(TAG, "Connection failed: %s (errno %d)", strerror(so_error), so_error);
        break;
      }
    } else if (sel_ret < 0) {
      ESP_LOGE(TAG, "Select error: %s", strerror(errno));
      break;
    }

    // Feed watchdog and yield
    esp_task_wdt_reset();
    vTaskDelay(1);
  }

  if (!connected) {
    ESP_LOGE(TAG, "Connection timeout");
    close(this->rtsp_socket_);
    this->rtsp_socket_ = -1;
    return false;
  }

  // Set back to blocking for RTSP commands
  fcntl(this->rtsp_socket_, F_SETFL, flags);

  // Set read/write timeout
  struct timeval tv;
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  setsockopt(this->rtsp_socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(this->rtsp_socket_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  ESP_LOGI(TAG, "TCP connection established");

  std::string full_url = "rtsp://" + host + ":" + std::to_string(port) + path;

  // OPTIONS
  if (!this->send_rtsp_request_("OPTIONS", full_url)) {
    this->disconnect_rtsp_stream_();
    return false;
  }

  // DESCRIBE
  std::string sdp_response;
  if (!this->send_rtsp_request_("DESCRIBE", full_url, "Accept: application/sdp\r\n", &sdp_response)) {
    this->disconnect_rtsp_stream_();
    return false;
  }

  // Pre-load SPS/PPS from the SDP (sprop-parameter-sets) so the decoder always
  // has its parameter sets, even if the camera only delivers them out-of-band or
  // via STAP-A. This is the most reliable source.
  {
    size_t sp = sdp_response.find("sprop-parameter-sets=");
    if (sp != std::string::npos) {
      sp += 21;  // strlen("sprop-parameter-sets=")
      size_t end = sdp_response.find_first_of(";\r\n", sp);
      std::string sprop = sdp_response.substr(sp, (end == std::string::npos ? sdp_response.size() : end) - sp);
      size_t comma = sprop.find(',');
      std::string sps_b64 = (comma == std::string::npos) ? sprop : sprop.substr(0, comma);
      std::string pps_b64 = (comma == std::string::npos) ? std::string() : sprop.substr(comma + 1);

      auto load = [](const std::string &b64, uint8_t *cache, size_t cache_size, size_t &out_len, bool &has) {
        if (b64.empty())
          return;
        unsigned char tmp[256];
        size_t olen = 0;
        if (mbedtls_base64_decode(tmp, sizeof(tmp), &olen, (const unsigned char *) b64.c_str(), b64.size()) == 0 &&
            olen > 0 && olen + 4 <= cache_size) {
          out_len = 0;
          cache[out_len++] = 0x00;
          cache[out_len++] = 0x00;
          cache[out_len++] = 0x00;
          cache[out_len++] = 0x01;
          memcpy(cache + out_len, tmp, olen);
          out_len += olen;
          has = true;
        }
      };
      load(sps_b64, this->sps_cache_, sizeof(this->sps_cache_), this->sps_len_, this->has_sps_);
      load(pps_b64, this->pps_cache_, sizeof(this->pps_cache_), this->pps_len_, this->has_pps_);
      this->param_sets_sent_ = false;
      this->param_sets_sent_fua_ = false;
      if (this->has_sps_ && this->has_pps_)
        ESP_LOGI(TAG, "Loaded SPS (%u) + PPS (%u) from SDP sprop-parameter-sets", this->sps_len_, this->pps_len_);

      // Inspect the SPS profile. The ESP32-P4 has NO hardware H.264 decoder; the
      // software decoder (tinyH264) supports CONSTRAINED BASELINE profile only.
      // Most IP cameras (Reolink, Hikvision, ...) stream Main/High profile, which
      // cannot be decoded here even though it plays fine in VLC.
      if (this->sps_len_ >= 8) {
        uint8_t profile_idc = this->sps_cache_[5];  // [00 00 00 01][nal][profile_idc][flags][level]
        uint8_t level_idc = this->sps_cache_[7];
        const char *pname = profile_idc == 66 ? "Baseline" : profile_idc == 77 ? "Main"
                            : profile_idc == 88 ? "Extended" : profile_idc == 100 ? "High"
                            : profile_idc == 110 ? "High10" : profile_idc == 122 ? "High422"
                            : profile_idc == 244 ? "High444" : "Unknown";
        ESP_LOGI(TAG, "H264 stream profile_idc=%u (%s), level_idc=%u", profile_idc, pname, level_idc);
        if (profile_idc != 66) {
#ifdef USE_H264_HP_EDGE264
          // edge264 gère Baseline/Main/High : on route ce flux vers lui.
          this->use_hp_decoder_ = true;
          ESP_LOGI(TAG, "H264 %s profile -> decoding via edge264 (h264_hp, High Profile capable).", pname);
#else
          ESP_LOGE(TAG, "This stream is H264 %s profile, but the ESP32-P4 software decoder "
                        "(tinyH264) only supports BASELINE. It will NOT decode (VLC works because "
                        "it has a full decoder).", pname);
          ESP_LOGE(TAG, "Fix: build libedge264.a (h264_hp) for native High decode, use MJPEG via "
                        "go2rtc, or transcode the stream to H264 Baseline.");
#endif
        }
      }
    }
  }

  // Parse SDP to get the control URL for the VIDEO media. Cameras often place a
  // session-level "a=control:*" before the per-track control, so we must look
  // for the control that belongs to the "m=video" section, not just the first one.
  std::string control_url = full_url;
  size_t mvideo = sdp_response.find("m=video");
  size_t control_pos = sdp_response.find("a=control:", mvideo != std::string::npos ? mvideo : 0);

  // Make sure the control we found is still inside the video section (before the
  // next "m=" media line); otherwise fall back to the first control in the SDP.
  if (control_pos != std::string::npos && mvideo != std::string::npos) {
    size_t next_m = sdp_response.find("\nm=", mvideo + 1);
    if (next_m != std::string::npos && control_pos > next_m)
      control_pos = std::string::npos;
  }
  if (control_pos == std::string::npos)
    control_pos = sdp_response.find("a=control:");

  if (control_pos != std::string::npos) {
    size_t start = control_pos + 10;  // length of "a=control:"
    size_t end = sdp_response.find_first_of("\r\n", start);
    std::string control = sdp_response.substr(start, (end == std::string::npos ? sdp_response.size() : end) - start);

    // Remove ALL whitespace characters (spaces, tabs, newlines)
    control.erase(std::remove_if(control.begin(), control.end(),
                                 [](unsigned char c) { return std::isspace(c); }),
                  control.end());

    ESP_LOGI(TAG, "SDP control attribute (cleaned): '%s'", control.c_str());

    if (control.empty() || control == "*") {
      // Aggregate control: use the DESCRIBE URL as-is
      control_url = full_url;
    } else if (control.find("://") != std::string::npos) {
      // Absolute URL
      control_url = control;
    } else if (control[0] == '/') {
      // Relative to server root
      size_t scheme_end = full_url.find("://");
      size_t path_start = (scheme_end != std::string::npos) ? full_url.find('/', scheme_end + 3) : std::string::npos;
      control_url = (path_start != std::string::npos) ? full_url.substr(0, path_start) + control : full_url + control;
    } else {
      // Relative to the current path
      control_url = full_url + (full_url.back() == '/' ? "" : "/") + control;
    }
    ESP_LOGI(TAG, "Using control URL from SDP: %s", control_url.c_str());
  } else {
    ESP_LOGW(TAG, "No control URL in SDP, using base URL");
    control_url = full_url;
  }

  // SETUP with TCP interleaved transport
  if (!this->send_rtsp_request_("SETUP", control_url, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")) {
    this->disconnect_rtsp_stream_();
    return false;
  }

  // PLAY
  char session_header[128];
  snprintf(session_header, sizeof(session_header), "Session: %s\r\n", this->rtsp_session_.c_str());
  if (!this->send_rtsp_request_("PLAY", full_url, session_header)) {
    this->disconnect_rtsp_stream_();
    return false;
  }

  // Set socket to non-blocking for reading interleaved data
  flags = fcntl(this->rtsp_socket_, F_GETFL, 0);
  fcntl(this->rtsp_socket_, F_SETFL, flags | O_NONBLOCK);

  this->stream_connected_ = true;
  ESP_LOGI(TAG, "RTSP stream connected (TCP interleaved)");

  return true;
}

void IPCameraViewer::disconnect_rtsp_stream_() {
  if (this->rtsp_socket_ >= 0) {
    // Send TEARDOWN
    if (!this->rtsp_session_.empty()) {
      // Set blocking for TEARDOWN
      int flags = fcntl(this->rtsp_socket_, F_GETFL, 0);
      fcntl(this->rtsp_socket_, F_SETFL, flags & ~O_NONBLOCK);

      // Best-effort TEARDOWN: send it but do not parse the reply. The socket
      // still carries interleaved RTP data ('$'), so there is no clean RTSP
      // response to read - trying would just log a spurious error.
      std::string req = "TEARDOWN " + this->url_ + " RTSP/1.0\r\n" +
                        "CSeq: " + std::to_string(this->cseq_++) + "\r\n" +
                        this->build_rtsp_auth_header_("TEARDOWN", this->url_) +
                        "Session: " + this->rtsp_session_ + "\r\n\r\n";
      send(this->rtsp_socket_, req.data(), req.size(), 0);
    }
    close(this->rtsp_socket_);
    this->rtsp_socket_ = -1;
  }
  this->stream_connected_ = false;
  this->rtsp_session_.clear();

  // CRITICAL: Reset SPS/PPS sent flags so they get sent again on reconnect
  this->param_sets_sent_ = false;
  this->param_sets_sent_fua_ = false;
  this->has_sps_ = false;
  this->has_pps_ = false;
  this->h264_data_len_ = 0;
  this->rtp_acc_len_ = 0;  // drop any half-received interleaved packet
}

// Compute the lowercase hex MD5 of a string (used for Digest auth)
static std::string md5_hex(const std::string &data) {
  unsigned char digest[16];
  mbedtls_md5(reinterpret_cast<const unsigned char *>(data.data()), data.size(), digest);
  static const char *const hexd = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (int i = 0; i < 16; i++) {
    out += hexd[digest[i] >> 4];
    out += hexd[digest[i] & 0x0F];
  }
  return out;
}

// Extract a parameter value from a WWW-Authenticate header, e.g. realm="...".
// Handles both quoted ("value") and bare (value) forms, matching only on a
// token boundary so that, e.g., "nonce" does not match inside "cnonce".
static std::string digest_param(const std::string &header, const std::string &key) {
  const std::string needle = key + "=";
  size_t pos = header.find(needle);
  while (pos != std::string::npos) {
    char before = (pos == 0) ? ' ' : header[pos - 1];
    if (before == ' ' || before == ',' || before == '\t')
      break;
    pos = header.find(needle, pos + 1);
  }
  if (pos == std::string::npos)
    return "";
  pos += needle.size();
  if (pos >= header.size())
    return "";
  if (header[pos] == '"') {
    size_t end = header.find('"', pos + 1);
    if (end == std::string::npos)
      return "";
    return header.substr(pos + 1, end - pos - 1);
  }
  size_t end = header.find_first_of(",\r\n ", pos);
  if (end == std::string::npos)
    end = header.size();
  return header.substr(pos, end - pos);
}

// Build the Authorization header line (with trailing CRLF). Uses Digest if a
// challenge has been received, otherwise falls back to Basic. Empty if no creds.
std::string IPCameraViewer::build_rtsp_auth_header_(const std::string &method, const std::string &uri) {
  if (!this->digest_nonce_.empty() && !this->rtsp_user_.empty()) {
    std::string ha1 = md5_hex(this->rtsp_user_ + ":" + this->digest_realm_ + ":" + this->rtsp_pass_);
    std::string ha2 = md5_hex(method + ":" + uri);
    std::string resp;
    std::string hdr = "Authorization: Digest username=\"" + this->rtsp_user_ + "\", realm=\"" +
                      this->digest_realm_ + "\", nonce=\"" + this->digest_nonce_ + "\", uri=\"" + uri + "\"";
    if (!this->digest_qop_.empty()) {
      this->digest_nc_++;
      char nc[9];
      snprintf(nc, sizeof(nc), "%08x", this->digest_nc_);
      std::string cnonce = md5_hex(std::to_string(millis()) + ":" + this->rtsp_user_).substr(0, 16);
      resp = md5_hex(ha1 + ":" + this->digest_nonce_ + ":" + nc + ":" + cnonce + ":auth:" + ha2);
      hdr += ", qop=auth, nc=" + std::string(nc) + ", cnonce=\"" + cnonce + "\"";
    } else {
      resp = md5_hex(ha1 + ":" + this->digest_nonce_ + ":" + ha2);
    }
    hdr += ", response=\"" + resp + "\"";
    if (!this->digest_opaque_.empty())
      hdr += ", opaque=\"" + this->digest_opaque_ + "\"";
    hdr += "\r\n";
    return hdr;
  }
  if (!this->rtsp_auth_.empty())
    return "Authorization: Basic " + this->rtsp_auth_ + "\r\n";
  return "";
}

bool IPCameraViewer::send_rtsp_request_(const std::string &method, const std::string &url,
                                       const std::string &extra_headers, std::string *response_body) {
  char response[4096];  // Large enough for SDP content
  std::string resp_str;

  // Up to 2 attempts: the first may return 401 with a Digest challenge, which we
  // parse and answer on the second attempt.
  for (int attempt = 0; attempt < 2; attempt++) {
    std::string auth_header = this->build_rtsp_auth_header_(method, url);

    // Build the request with std::string (a Digest header can exceed 768 bytes)
    std::string request = method + " " + url + " RTSP/1.0\r\n" +
                          "CSeq: " + std::to_string(this->cseq_++) + "\r\n" +
                          auth_header + extra_headers + "\r\n";

    if (send(this->rtsp_socket_, request.data(), request.size(), 0) < 0) {
      ESP_LOGE(TAG, "Failed to send RTSP %s", method.c_str());
      return false;
    }

    // PLAY: do NOT bulk-read here. Right after PLAY the camera streams interleaved
    // RTP ('$') on this same TCP socket; a recv() of many bytes would swallow part
    // of the first RTP packet and desync the interleaved framing (no frames would
    // ever assemble). Read ONLY the RTSP response - or nothing if the stream has
    // already started - leaving all stream data for the RTP reader.
    if (method == "PLAY") {
      char first = 0;
      if (recv(this->rtsp_socket_, &first, 1, MSG_PEEK) > 0 && first == '$') {
        ESP_LOGI(TAG, "RTSP PLAY OK (stream already flowing)");
        return true;
      }
      std::string play_resp;
      char c;
      int guard = 0;
      while (play_resp.find("\r\n\r\n") == std::string::npos && guard++ < 4096) {
        if (recv(this->rtsp_socket_, &c, 1, 0) <= 0)
          break;
        play_resp += c;
      }
      if (play_resp.find(" 200") == std::string::npos) {
        ESP_LOGE(TAG, "RTSP PLAY failed: %s", play_resp.c_str());
        return false;
      }
      ESP_LOGI(TAG, "RTSP PLAY OK");
      return true;
    }

    int len = recv(this->rtsp_socket_, response, sizeof(response) - 1, 0);
    if (len <= 0) {
      ESP_LOGE(TAG, "Failed to receive RTSP response");
      return false;
    }
    response[len] = '\0';
    resp_str.assign(response, len);

    // Handle 401 Unauthorized
    if (resp_str.find(" 401") != std::string::npos) {
      bool has_digest = resp_str.find("Digest") != std::string::npos ||
                        resp_str.find("digest") != std::string::npos;
      if (attempt == 0 && !this->rtsp_user_.empty() && has_digest) {
        // Parse the Digest challenge and retry with a computed response
        this->digest_realm_ = digest_param(resp_str, "realm");
        this->digest_nonce_ = digest_param(resp_str, "nonce");
        this->digest_qop_ = digest_param(resp_str, "qop");
        this->digest_opaque_ = digest_param(resp_str, "opaque");
        this->digest_nc_ = 0;
        ESP_LOGI(TAG, "RTSP %s: got Digest challenge (realm=\"%s\"), retrying with authentication",
                 method.c_str(), this->digest_realm_.c_str());
        continue;
      }
      // Could not authenticate: give a precise reason to help diagnosis
      if (this->rtsp_user_.empty()) {
        ESP_LOGE(TAG, "RTSP %s 401: the camera requires authentication but no credentials were "
                      "provided. Use a URL like rtsp://user:pass@host:port/path", method.c_str());
      } else {
        size_t wpos = resp_str.find("WWW-Authenticate:");
        std::string www = (wpos != std::string::npos)
                              ? resp_str.substr(wpos, resp_str.find('\n', wpos) - wpos)
                              : std::string("(no WWW-Authenticate header)");
        ESP_LOGE(TAG, "RTSP %s 401 unauthorized (wrong user/password, or unsupported scheme). "
                      "Challenge: %s", method.c_str(), www.c_str());
      }
      return false;
    }

    break;  // Non-401 response: stop retrying and evaluate below
  }

  // Check status
  if (resp_str.find("200 OK") == std::string::npos) {
    ESP_LOGE(TAG, "RTSP %s failed: %s", method.c_str(), resp_str.c_str());
    return false;
  }

  // Extract Session ID from SETUP response
  if (method == "SETUP") {
    size_t spos = resp_str.find("Session: ");
    if (spos != std::string::npos) {
      spos += 9;
      size_t end = resp_str.find_first_of(";\r\n", spos);
      if (end != std::string::npos) {
        this->rtsp_session_ = resp_str.substr(spos, end - spos);
        ESP_LOGI(TAG, "RTSP Session: %s", this->rtsp_session_.c_str());
      }
    }
  }

  // If caller wants the response body (for SDP parsing)
  if (response_body != nullptr) {
    *response_body = resp_str;
  }

  ESP_LOGI(TAG, "RTSP %s OK", method.c_str());
  return true;
}

bool IPCameraViewer::fetch_rtp_frame_() {
  if (this->rtsp_socket_ < 0) {
    return false;
  }

  // TCP interleaved format:
  // $ (0x24), channel (1 byte), length (2 bytes big endian), RTP data
  // Packets are extracted from the persistent rtp_acc_ buffer (see below).
  uint8_t rtp_packet[1500];

  // --- Régulateur de latence (voir ip_camera_viewer.h) ----------------------
  // Si le socket reste saturé plusieurs contrôles de suite, le décodage est en
  // retard durable sur la caméra (P-frames lourdes : nuit IR, mouvement) : on
  // passe en rattrapage — les P-frames sont drainées SANS décodage jusqu'à la
  // prochaine IDR, puis on reprend en direct. Sans ça, le retard s'accumule
  // sans limite (flux "au ralenti", minutes d'écart avec le direct).
  uint32_t now_catchup = millis();
  if (now_catchup - this->catchup_last_check_ >= 500) {
    this->catchup_last_check_ = now_catchup;
    int pending = 0;
    if (ioctl(this->rtsp_socket_, FIONREAD, &pending) < 0)
      pending = 0;
    // Seuil : la fenêtre TCP lwip plafonne FIONREAD (souvent ~5,7 Ko) — 4 Ko en
    // attente permanente = le décodeur ne draine plus, le reste s'empile côté
    // caméra (invisible d'ici). Quand le décodage suit, le socket se vide entre
    // deux frames et le compteur retombe à zéro.
    if ((size_t) pending + this->rtp_acc_len_ > 4096) {
      if (this->catchup_full_ticks_ < 255)
        this->catchup_full_ticks_++;
    } else {
      this->catchup_full_ticks_ = 0;
    }
    // ~2 s de saturation continue (4 contrôles) : une simple rafale IDR (~15 Ko,
    // drainée en <1 s) ne déclenche pas ; un retard structurel, oui.
    if (!this->catchup_skip_to_idr_ && this->catchup_full_ticks_ >= 4) {
      this->catchup_skip_to_idr_ = true;
      this->h264_data_len_ = 0;  // jeter la frame partiellement assemblée
      ESP_LOGW(TAG, "Decoder falling behind the camera (socket saturated >2 s) — skipping "
                    "P-frames until the next IDR to stay live.");
    }
  }

  // Accumulate NAL units into h264_buffer_
  bool frame_complete = false;

  // --- Diagnostic RTP (visible au niveau INFO toutes les ~100 lectures) -------
  // Révèle POURQUOI aucune frame ne se forme : pas de data (eagain), framing
  // interleaved KO (non '$'), mauvais canal, ou marker jamais vu.
  static uint32_t d_calls = 0, d_eagain = 0, d_short = 0, d_nondollar = 0;
  static uint32_t d_pkts = 0, d_bytes = 0, d_ch0 = 0, d_markers = 0;
  static uint32_t d_nal_mask = 0;  // bit i = nal_type i vu
  d_calls++;
  if (d_calls % 2000 == 0) {
    // NAL types RTP : 1=P,5=IDR,7=SPS,8=PPS,24=STAP-A,28=FU-A
    ESP_LOGD(TAG,
             "RTP diag: calls=%u pkts=%u bytes=%u ch0=%u markers=%u | "
             "eagain=%u short=%u nondollar=%u | nal_mask=0x%08x",
             d_calls, d_pkts, d_bytes, d_ch0, d_markers, d_eagain, d_short,
             d_nondollar, d_nal_mask);
  }

  while (!frame_complete) {
    // 1) Drain whatever is available from the socket into a PERSISTENT
    //    accumulation buffer. Over TCP the camera's interleaved frames split
    //    arbitrarily across recv()s, and on a NON-blocking socket recv() returns
    //    only the bytes that have arrived so far. The previous code peeked+
    //    consumed the 4-byte interleaved header and then broke if the payload
    //    wasn't fully there yet — leaving the '$' framing desynced; stray 0x24
    //    bytes inside the H.264 payload then re-synced to bogus packets and NO
    //    frame ever assembled (the "started=0 / frames=0" symptom). We now only
    //    ever parse COMPLETE '$'-framed packets and keep partial bytes for the
    //    next tick. (Validated on host with trickle delivery: 0 -> 20 frames.)
    if (this->rtp_acc_len_ < sizeof(this->rtp_acc_)) {
      ssize_t len = recv(this->rtsp_socket_, this->rtp_acc_ + this->rtp_acc_len_,
                         sizeof(this->rtp_acc_) - this->rtp_acc_len_, 0);
      if (len > 0) {
        this->rtp_acc_len_ += (size_t) len;
      } else if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        d_eagain++;  // no new data this tick
      } else {
        // Socket genuinely dead (closed by peer, reset, ...). Close it and
        // mark disconnected so the NEXT enable properly reconnects instead of
        // trying (and failing forever) to reuse a broken socket — this matters
        // most for keep_alive, whose fast path in loop() only skips
        // reconnection while stream_connected_ is actually true.
        close(this->rtsp_socket_);
        this->rtsp_socket_ = -1;
        this->stream_connected_ = false;
        return false;
      }
    }

    // 2) Resync to the next interleaved marker '$'.
    while (this->rtp_acc_len_ > 0 && this->rtp_acc_[0] != '$') {
      memmove(this->rtp_acc_, this->rtp_acc_ + 1, --this->rtp_acc_len_);
      d_nondollar++;
    }
    if (this->rtp_acc_len_ < 4) {
      d_short++;
      break;  // header not fully arrived yet — keep what we have for next tick
    }

    uint8_t channel = this->rtp_acc_[1];
    uint16_t rtp_len = (this->rtp_acc_[2] << 8) | this->rtp_acc_[3];

    if (rtp_len == 0 || rtp_len > sizeof(rtp_packet)) {
      // Bogus length (almost certainly a false '$' inside a payload): drop this
      // byte and resync rather than trusting it.
      memmove(this->rtp_acc_, this->rtp_acc_ + 1, --this->rtp_acc_len_);
      continue;
    }

    if ((size_t) 4 + rtp_len > this->rtp_acc_len_) {
      break;  // full packet not arrived yet — wait for the next tick
    }

    // 3) We have exactly ONE complete interleaved packet. Copy it out and
    //    remove it (header + payload) from the accumulation buffer.
    memcpy(rtp_packet, this->rtp_acc_ + 4, rtp_len);
    const size_t consumed = (size_t) 4 + rtp_len;
    memmove(this->rtp_acc_, this->rtp_acc_ + consumed, this->rtp_acc_len_ - consumed);
    this->rtp_acc_len_ -= consumed;

    d_pkts++;
    d_bytes += rtp_len;

    // Skip RTCP packets (channel 1)
    if (channel != 0) {
      continue;
    }
    d_ch0++;

    if (rtp_len < 12) {
      continue;  // Invalid RTP packet
    }

    // RTP header
    uint8_t marker = (rtp_packet[1] >> 7) & 0x01;
    if (marker)
      d_markers++;

    int header_len = 12;  // Basic RTP header

    // H264 NAL unit starts after RTP header
    uint8_t *nal_data = rtp_packet + header_len;
    int nal_len = rtp_len - header_len;

    if (nal_len <= 0) {
      continue;
    }

    // Check NAL unit type
    uint8_t nal_type = nal_data[0] & 0x1F;
    if (nal_type < 32)
      d_nal_mask |= (1u << nal_type);

    // H.264 NAL unit types:
    // 7 = SPS (Sequence Parameter Set)
    // 8 = PPS (Picture Parameter Set)
    // 5 = IDR (I-frame)
    // 1 = Non-IDR (P-frame)

    if (nal_type == 7) {
      // SPS - cache it (with start code)
      if (nal_len + 4 <= sizeof(this->sps_cache_)) {
        this->sps_len_ = 0;
        this->sps_cache_[this->sps_len_++] = 0x00;
        this->sps_cache_[this->sps_len_++] = 0x00;
        this->sps_cache_[this->sps_len_++] = 0x00;
        this->sps_cache_[this->sps_len_++] = 0x01;
        memcpy(this->sps_cache_ + this->sps_len_, nal_data, nal_len);
        this->sps_len_ += nal_len;
        this->has_sps_ = true;
        ESP_LOGD(TAG, "Cached SPS: %u bytes", this->sps_len_);
      }
      // Don't add SPS to main buffer - it will be prepended to I-frames
    } else if (nal_type == 8) {
      // PPS - cache it (with start code)
      if (nal_len + 4 <= sizeof(this->pps_cache_)) {
        this->pps_len_ = 0;
        this->pps_cache_[this->pps_len_++] = 0x00;
        this->pps_cache_[this->pps_len_++] = 0x00;
        this->pps_cache_[this->pps_len_++] = 0x00;
        this->pps_cache_[this->pps_len_++] = 0x01;
        memcpy(this->pps_cache_ + this->pps_len_, nal_data, nal_len);
        this->pps_len_ += nal_len;
        this->has_pps_ = true;
        ESP_LOGD(TAG, "Cached PPS: %u bytes", this->pps_len_);
      }
      // Don't add PPS to main buffer - it will be prepended to I-frames
    } else if (nal_type >= 1 && nal_type <= 23) {
      // Picture NAL unit (I-frame, P-frame, etc.)

      // Latency catch-up: P-frames are drained without decoding until an IDR.
      if (this->catchup_skip_to_idr_) {
        if (nal_type == 5) {
          this->catchup_skip_to_idr_ = false;
          // Still backlogged? Decode this IDR (fresh picture) but re-arm in
          // 0.5 s (instead of 2 s) to skip most of the next GOP too. Without
          // this, resuming FULL decoding at the first IDR fell behind again
          // immediately and latency plateaued at ~30-60 s instead of
          // converging.
          int pend = 0;
          if (ioctl(this->rtsp_socket_, FIONREAD, &pend) < 0)
            pend = 0;
          if ((size_t) pend + this->rtp_acc_len_ > 4096) {
            this->catchup_full_ticks_ = 3;
            // DEBUG: can repeat every GOP while the backlog persists.
            ESP_LOGD(TAG, "IDR decoded but still behind (%d bytes pending) — catch-up kept "
                          "active.", pend);
          } else {
            this->catchup_full_ticks_ = 0;
            ESP_LOGI(TAG, "IDR reached — resuming live decoding.");
          }
        } else {
          continue;  // P-frame jetée (consommée du buffer, jamais décodée)
        }
      }

      // CRITICAL FIX: Send SPS/PPS with the FIRST frame received (not just I-frames)
      // Without this, if stream starts with P-frames, decoder never gets param sets
      if (!this->param_sets_sent_ && this->has_sps_ && this->has_pps_) {
        // Send SPS/PPS with first frame (I-frame OR P-frame)
        if (this->h264_data_len_ + this->sps_len_ + this->pps_len_ + nal_len + 4 < this->h264_buffer_size_) {
          // Add SPS
          memcpy(this->h264_buffer_ + this->h264_data_len_, this->sps_cache_, this->sps_len_);
          this->h264_data_len_ += this->sps_len_;
          // Add PPS
          memcpy(this->h264_buffer_ + this->h264_data_len_, this->pps_cache_, this->pps_len_);
          this->h264_data_len_ += this->pps_len_;

          ESP_LOGI(TAG, "Sent SPS+PPS (%u+%u bytes) with FIRST frame (NAL type %u)",
                   this->sps_len_, this->pps_len_, nal_type);
          this->param_sets_sent_ = true;
        }
      }

      // Also prepend SPS/PPS to each I-frame for recovery after packet loss
      if (nal_type == 5 && this->has_sps_ && this->has_pps_ && this->param_sets_sent_) {
        if (this->h264_data_len_ + this->sps_len_ + this->pps_len_ + nal_len + 4 < this->h264_buffer_size_) {
          // Add SPS
          memcpy(this->h264_buffer_ + this->h264_data_len_, this->sps_cache_, this->sps_len_);
          this->h264_data_len_ += this->sps_len_;
          // Add PPS
          memcpy(this->h264_buffer_ + this->h264_data_len_, this->pps_cache_, this->pps_len_);
          this->h264_data_len_ += this->pps_len_;
          ESP_LOGI(TAG, "Prepended SPS+PPS (%u+%u bytes) to I-frame for recovery", this->sps_len_, this->pps_len_);
        }
      }

      // Add the picture NAL unit itself
      if (this->h264_data_len_ + nal_len + 4 < this->h264_buffer_size_) {
        // Add start code
        this->h264_buffer_[this->h264_data_len_++] = 0x00;
        this->h264_buffer_[this->h264_data_len_++] = 0x00;
        this->h264_buffer_[this->h264_data_len_++] = 0x00;
        this->h264_buffer_[this->h264_data_len_++] = 0x01;
        memcpy(this->h264_buffer_ + this->h264_data_len_, nal_data, nal_len);
        this->h264_data_len_ += nal_len;

        // Log first 10 frames for debugging
        static uint32_t frame_count = 0;
        if (frame_count++ < 10) {
          const char *frame_type = nal_type == 5 ? "I-frame (IDR)" :
                                   nal_type == 1 ? "P-frame" :
                                   nal_type == 2 ? "P-frame (partition A)" :
                                   nal_type == 3 ? "P-frame (partition B)" :
                                   nal_type == 4 ? "P-frame (partition C)" : "Other";
          ESP_LOGI(TAG, "Frame #%u: NAL type %u (%s), size %u bytes",
                   frame_count, nal_type, frame_type, nal_len);
        }
      }
    } else if (nal_type == 24) {
      // STAP-A: a single RTP packet aggregating several NAL units, commonly used
      // to carry SPS + PPS together. Layout: [STAP-A hdr][size(2)][NAL]...[size(2)][NAL]
      int offset = 1;  // skip the STAP-A header byte
      while (offset + 2 <= nal_len) {
        uint16_t unit_size = (nal_data[offset] << 8) | nal_data[offset + 1];
        offset += 2;
        if (unit_size == 0 || offset + unit_size > nal_len)
          break;
        uint8_t *unit = nal_data + offset;
        uint8_t utype = unit[0] & 0x1F;
        if (utype == 7 && unit_size + 4 <= (int) sizeof(this->sps_cache_)) {
          this->sps_len_ = 0;
          this->sps_cache_[this->sps_len_++] = 0x00;
          this->sps_cache_[this->sps_len_++] = 0x00;
          this->sps_cache_[this->sps_len_++] = 0x00;
          this->sps_cache_[this->sps_len_++] = 0x01;
          memcpy(this->sps_cache_ + this->sps_len_, unit, unit_size);
          this->sps_len_ += unit_size;
          this->has_sps_ = true;
          ESP_LOGD(TAG, "Cached SPS from STAP-A: %u bytes", this->sps_len_);
        } else if (utype == 8 && unit_size + 4 <= (int) sizeof(this->pps_cache_)) {
          this->pps_len_ = 0;
          this->pps_cache_[this->pps_len_++] = 0x00;
          this->pps_cache_[this->pps_len_++] = 0x00;
          this->pps_cache_[this->pps_len_++] = 0x00;
          this->pps_cache_[this->pps_len_++] = 0x01;
          memcpy(this->pps_cache_ + this->pps_len_, unit, unit_size);
          this->pps_len_ += unit_size;
          this->has_pps_ = true;
          ESP_LOGD(TAG, "Cached PPS from STAP-A: %u bytes", this->pps_len_);
        } else if (this->h264_data_len_ + unit_size + 4 < this->h264_buffer_size_) {
          // Any other aggregated NAL (e.g. SEI): append with a start code
          this->h264_buffer_[this->h264_data_len_++] = 0x00;
          this->h264_buffer_[this->h264_data_len_++] = 0x00;
          this->h264_buffer_[this->h264_data_len_++] = 0x00;
          this->h264_buffer_[this->h264_data_len_++] = 0x01;
          memcpy(this->h264_buffer_ + this->h264_data_len_, unit, unit_size);
          this->h264_data_len_ += unit_size;
        }
        offset += unit_size;
      }
    } else if (nal_type == 28) {
      // FU-A (Fragmentation Unit)
      if (nal_len < 2) continue;

      uint8_t fu_header = nal_data[1];
      bool start = (fu_header >> 7) & 0x01;
      uint8_t fu_type = fu_header & 0x1F;

      // Latency catch-up: fragments are dropped until an IDR start (FU type 5).
      if (this->catchup_skip_to_idr_) {
        if (start && fu_type == 5) {
          this->catchup_skip_to_idr_ = false;
          // See the single-NAL path above: IDR decoded, but re-arm in 0.5 s
          // if the backlog persists, to keep skipping P-frames of the next GOP.
          int pend = 0;
          if (ioctl(this->rtsp_socket_, FIONREAD, &pend) < 0)
            pend = 0;
          if ((size_t) pend + this->rtp_acc_len_ > 4096) {
            this->catchup_full_ticks_ = 3;
            // DEBUG: can repeat every GOP while the backlog persists.
            ESP_LOGD(TAG, "IDR decoded but still behind (%d bytes pending) — catch-up kept "
                          "active.", pend);
          } else {
            this->catchup_full_ticks_ = 0;
            ESP_LOGI(TAG, "IDR reached — resuming live decoding.");
          }
        } else {
          continue;  // fragment jeté (consommé du buffer, jamais décodé)
        }
      }

      if (start) {
        // Start of fragmented NAL
        uint8_t reconstructed = (nal_data[0] & 0xE0) | fu_type;

        // CRITICAL FIX: Send SPS/PPS with first fragmented frame (but not twice!)
        // Only prepend SPS/PPS if we haven't sent them yet
        // This handles both first frame AND subsequent I-frames
        if (!this->param_sets_sent_fua_ && this->has_sps_ && this->has_pps_ && (fu_type >= 1 && fu_type <= 23)) {
          // Send SPS/PPS with first fragmented picture frame
          if (this->h264_data_len_ + this->sps_len_ + this->pps_len_ + nal_len + 3 < this->h264_buffer_size_) {
            // Add SPS
            memcpy(this->h264_buffer_ + this->h264_data_len_, this->sps_cache_, this->sps_len_);
            this->h264_data_len_ += this->sps_len_;
            // Add PPS
            memcpy(this->h264_buffer_ + this->h264_data_len_, this->pps_cache_, this->pps_len_);
            this->h264_data_len_ += this->pps_len_;
            ESP_LOGI(TAG, "Sent SPS+PPS (%u+%u bytes) with FIRST fragmented frame (FU type %u)",
                     this->sps_len_, this->pps_len_, fu_type);
            this->param_sets_sent_fua_ = true;
          } else {
            ESP_LOGW(TAG, "Buffer too small to add SPS+PPS (need %u bytes, buffer has %u free)",
                     this->sps_len_ + this->pps_len_ + nal_len + 3,
                     this->h264_buffer_size_ - this->h264_data_len_);
          }
        }
        // Note: We removed the second SPS/PPS prepending block to avoid double prepending
        // I-frames will get SPS/PPS from the first block when param_sets_sent_fua is false

        if (this->h264_data_len_ + nal_len + 3 < this->h264_buffer_size_) {
          this->h264_buffer_[this->h264_data_len_++] = 0x00;
          this->h264_buffer_[this->h264_data_len_++] = 0x00;
          this->h264_buffer_[this->h264_data_len_++] = 0x00;
          this->h264_buffer_[this->h264_data_len_++] = 0x01;
          this->h264_buffer_[this->h264_data_len_++] = reconstructed;
          memcpy(this->h264_buffer_ + this->h264_data_len_, nal_data + 2, nal_len - 2);
          this->h264_data_len_ += nal_len - 2;

          // Log first 10 fragmented frames for debugging
          static uint32_t frag_count = 0;
          if (frag_count++ < 10) {
            const char *frame_type = fu_type == 5 ? "I-frame (IDR)" :
                                     fu_type == 1 ? "P-frame" : "Other";
            ESP_LOGI(TAG, "Fragmented frame #%u: FU type %u (%s), fragment size %u bytes",
                     frag_count, fu_type, frame_type, nal_len - 2);
          }
        }
      } else {
        // Continuation
        if (this->h264_data_len_ + nal_len - 2 < this->h264_buffer_size_) {
          memcpy(this->h264_buffer_ + this->h264_data_len_, nal_data + 2, nal_len - 2);
          this->h264_data_len_ += nal_len - 2;
        }
      }
    }

    // Marker bit indicates end of frame
    // Only set frame_complete for actual picture data (not SPS/PPS)
    if (marker && nal_type != 7 && nal_type != 8) {
      frame_complete = true;
    }
  }

  if (frame_complete && this->h264_data_len_ > 0) {
    if (this->first_update_) {
      ESP_LOGI(TAG, "First H264 frame: %u bytes", this->h264_data_len_);
      this->first_update_ = false;
    }
    return true;
  }

  return false;
}

bool IPCameraViewer::decode_h264_to_yuv_() {
  if (this->h264_data_len_ == 0) {
    return false;
  }

#ifdef USE_H264_HP_EDGE264
  if (this->use_hp_decoder_) {
    if (!this->hp_started_) {
      // MONO-THREAD (begin(0)) : décodage SYNCHRONE, AUCUN thread worker.
      // En multi-thread (begin 1/2), edge264 crée des pthreads ; sur les pthreads
      // FreeRTOS d'ESP-IDF, loopTask se bloque dans decode_NAL/get_frame à attendre
      // un worker qui n'avance plus (les 2 cœurs retombent IDLE) -> reboot par le
      // Task Watchdog (loopTask affamée). Les pthreads Linux ne reproduisaient pas
      // ce deadlock. begin(0) supprime toute la couche threads : decode_NAL décode
      // en ligne et get_frame renvoie aussitôt (mode prouvé 30/30 sur PC). Le
      // décodage (~20 Ko de pile) tourne sur loopTask : il faut donc
      // loop_task_stack_size: 32768 dans la config esp32 (sinon débordement).
      this->hp_started_ = this->hp_decoder_.begin(0);
      if (!this->hp_started_) {
        ESP_LOGE(TAG, "edge264: High Profile decoder initialization failed");
        this->h264_data_len_ = 0;
        return false;
      }
    }

    // Fournir le flux Annex-B accumulé (SPS/PPS + slices) à edge264.
    // DIAG perf : chrono ISOLÉ du décodage (µs) pour le séparer de la conversion
    // YUV->RGB et du rendu LVGL. Le "lvgl took a long time" englobe tout ; ici on
    // sait exactement combien coûte edge264 seul, et donc le temps par macrobloc.
    const int64_t _dec_t0 = esp_timer_get_time();
    this->hp_decoder_.decode_annexb(this->h264_buffer_, this->h264_data_len_);
    const int64_t _dec_us = esp_timer_get_time() - _dec_t0;
    // Seuil 100 ms : ne logge que les IDR et les anomalies. L'ancien seuil de
    // 3 ms loggait CHAQUE P-frame (~15 WARN/s) — le logger UART consommait un
    // temps CPU mesurable ("logger took a long time") et noyait les logs utiles.
    if (_dec_us > 100000)
      ESP_LOGW(TAG, "edge264 decode: %lld ms pour %u octets (%u MB config)",
               (long long) (_dec_us / 1000), (unsigned) this->h264_data_len_,
               (unsigned) ((this->width_ / 16) * (this->height_ / 16)));
    this->h264_data_len_ = 0;

    bool got = false;
    h264_hp::DecodedFrame f;
    while (this->hp_decoder_.get_frame(&f)) {
      const int sw = f.width & ~1;   // dimensions paires (4:2:0) du flux décodé
      const int sh = f.height & ~1;
      if (this->ppa_ok_ && this->ouyy_buffer_ != nullptr && sw == (int) this->width_ &&
          sh == (int) this->height_ && f.y && f.cb && f.cr) {
        // Chemin PPA : repack I420 (plans du DPB) -> O_UYY_E_VYY, FUSIONNÉ dans
        // la copie de sortie (remplace les memcpy du chemin planaire, coût
        // équivalent). Lignes paires "U Y Y...", impaires "V Y Y..." (format
        // YUV420 matériel du P4). La conversion RGB565 sera faite par le PPA.
        const size_t line3 = (size_t) this->width_ * 3 / 2;
        for (int row = 0; row < sh; row += 2) {
          const uint8_t *y0 = f.y + (size_t) row * f.stride_y;
          const uint8_t *y1 = y0 + f.stride_y;
          const uint8_t *u = f.cb + (size_t) (row >> 1) * f.stride_c;
          const uint8_t *v = f.cr + (size_t) (row >> 1) * f.stride_c;
          uint8_t *o0 = this->ouyy_buffer_ + (size_t) row * line3;
          uint8_t *o1 = o0 + line3;
          for (int x = 0, c = 0; x < sw; x += 2, c++) {
            *o0++ = u[c];
            *o0++ = y0[x];
            *o0++ = y0[x + 1];
            *o1++ = v[c];
            *o1++ = y1[x];
            *o1++ = y1[x + 1];
          }
        }
        this->yuv_is_ouyy_ = true;
        got = true;
        this->hp_decoder_.release_frame();
        continue;
      }
      this->yuv_is_ouyy_ = false;
      if (sw > 0 && sh > 0 && f.y && f.cb && f.cr) {
        // yuv_buffer_ et convert_yuv420_to_rgb565_ sont FIXÉS à width_/height_ (la
        // résolution du FLUX, config YAML) — le canvas, lui, peut être plus grand
        // si display_width_/height_ est configuré (voir render_width_()), mais ce
        // redimensionnement est une étape SÉPARÉE et ultérieure (PPA), qui ne
        // change rien ici. On dispose donc l'image décodée dans un plan I420 à la
        // taille CONFIGURÉE : on recadre (crop) si le flux est plus grand, on
        // letterbox (bords noirs) s'il est plus petit. Conséquence : un mismatch
        // de résolution donne TOUJOURS une image (jamais d'écran noir) et ne peut
        // JAMAIS déborder yuv_buffer_ (= width_*height_*3/2). Cas nominal (flux ==
        // config) : dw=width_, dh=height_ -> copie pleine, identique à avant.
        const int dw = (sw < (int) this->width_ ? sw : (int) this->width_) & ~1;
        const int dh = (sh < (int) this->height_ ? sh : (int) this->height_) & ~1;
        const int cfg_cw = this->width_ / 2, cfg_ch = this->height_ / 2;
        if (sw != (int) this->width_ || sh != (int) this->height_) {
          static bool warned = false;
          if (!warned) {
            ESP_LOGW(TAG, "edge264: stream %dx%d != config %ux%u — cropped/letterboxed. "
                          "Adjust width/height for full-frame rendering.",
                     sw, sh, this->width_, this->height_);
            warned = true;
          }
          // Flux plus petit que la config : noircir une fois le buffer pour que
          // les bords non remplis soient noirs (et non du contenu PSRAM résiduel).
          if (dw < (int) this->width_ || dh < (int) this->height_)
            memset(this->yuv_buffer_, 0, this->yuv_buffer_size_);
        }
        uint8_t *Y = this->yuv_buffer_;
        uint8_t *U = Y + (size_t) this->width_ * this->height_;
        uint8_t *V = U + (size_t) cfg_cw * cfg_ch;
        for (int row = 0; row < dh; row++)
          memcpy(Y + (size_t) row * this->width_, f.y + (size_t) row * f.stride_y, dw);
        for (int row = 0; row < dh / 2; row++)
          memcpy(U + (size_t) row * cfg_cw, f.cb + (size_t) row * f.stride_c, dw / 2);
        for (int row = 0; row < dh / 2; row++)
          memcpy(V + (size_t) row * cfg_cw, f.cr + (size_t) row * f.stride_c, dw / 2);
        got = true;
      }
      this->hp_decoder_.release_frame();
    }
    return got;
  }
#endif

  // Baseline path: lazily create the tinyH264/h264bsd decoder the first time we
  // actually need it. Deferred from setup() on purpose — its prebuilt RISC-V
  // worker faults ("Instruction address misaligned", core 1) on the ESP32-P4,
  // and a High-profile stream (routed to edge264 above) must never spawn it.
  if (this->h264_decoder_ == nullptr) {
    if (!this->init_h264_decoder_()) {
      this->h264_data_len_ = 0;
      return false;
    }
  }

  esp_h264_dec_in_frame_t in_frame = {};
  in_frame.raw_data.buffer = this->h264_buffer_;
  in_frame.raw_data.len = this->h264_data_len_;

  esp_h264_dec_out_frame_t out_frame = {};

  // Process all NAL units in the buffer
  bool frame_decoded = false;
  static bool first_decode_success = false;

  while (in_frame.raw_data.len > 0) {
    esp_h264_err_t ret = esp_h264_dec_process(this->h264_decoder_, &in_frame, &out_frame);
    if (ret != ESP_H264_ERR_OK) {
      // Log decode error for debugging
      static uint32_t error_count = 0;
      error_count++;
      if (error_count <= 10 || error_count % 100 == 0) {
        ESP_LOGE(TAG, "H264 decode error: %d (NAL size: %u bytes, error #%u)",
                 ret, in_frame.raw_data.len, error_count);

        // Explain error code
        if (ret == -1) ESP_LOGE(TAG, "  -> ESP_H264_ERR_FAIL (general decode failure)");
        if (ret == -2) ESP_LOGE(TAG, "  -> ESP_H264_ERR_ARG (invalid arguments)");
        if (ret == -3) ESP_LOGE(TAG, "  -> ESP_H264_ERR_MEM (out of memory)");
        if (ret == -5) ESP_LOGE(TAG, "  -> ESP_H264_ERR_UNSUPPORTED (profile incompatible or feature not supported)");
        if (ret == -6) ESP_LOGE(TAG, "  -> ESP_H264_ERR_TIMEOUT");
        if (ret == -7) ESP_LOGE(TAG, "  -> ESP_H264_ERR_OVERFLOW");

        if (!first_decode_success) {
          ESP_LOGE(TAG, "  No frames decoded yet - check if SPS/PPS were sent with first frame");
          ESP_LOGE(TAG, "  If error = -5, H264 profile may be incompatible (High Profile not fully supported)");
        }
      }
      break;
    }

    // Check if we got a decoded frame
    if (out_frame.out_size > 0 && out_frame.outbuf != nullptr) {
      // Copy decoded YUV data to our buffer
      size_t copy_size = out_frame.out_size;
      if (copy_size > this->yuv_buffer_size_) {
        copy_size = this->yuv_buffer_size_;
      }
      memcpy(this->yuv_buffer_, out_frame.outbuf, copy_size);
      frame_decoded = true;

      // Log first successful decode
      if (!first_decode_success) {
        ESP_LOGI(TAG, "First frame decoded successfully! Decoder initialized and working.");
        ESP_LOGI(TAG, "  Decoded YUV size: %u bytes (expected: %u bytes)",
                 out_frame.out_size, this->yuv_buffer_size_);
        first_decode_success = true;
      }
    }

    // Move to next NAL unit
    in_frame.raw_data.buffer += in_frame.consume;
    in_frame.raw_data.len -= in_frame.consume;
  }

  // Reset buffer for next frame
  this->h264_data_len_ = 0;

  return frame_decoded;
}

void IPCameraViewer::convert_yuv420_to_rgb565_(uint8_t *yuv, uint8_t *rgb565, int width, int height) {
  // YUV420 (I420) layout:
  // Y plane: width * height bytes
  // U plane: (width/2) * (height/2) bytes
  // V plane: (width/2) * (height/2) bytes
  //
  // Optimisé par blocs 2x2 : en 4:2:0 les 4 pixels d'un bloc partagent U/V, donc
  // les trois termes chroma (rc/gc/bc) ne sont calculés qu'UNE fois par bloc au
  // lieu d'une fois par pixel (3 multiplications économisées sur 4 pixels), et
  // les deux lignes du bloc sont écrites dans la même passe (localité PSRAM).
  // L'arithmétique par pixel est INCHANGÉE -> rendu strictement identique.
  const int cw = width >> 1;
  const uint8_t *y_plane = yuv;
  const uint8_t *u_plane = yuv + width * height;
  const uint8_t *v_plane = u_plane + cw * (height >> 1);
  uint16_t *rgb = (uint16_t *) rgb565;

  for (int j = 0; j < height; j += 2) {
    const uint8_t *y0 = y_plane + j * width;
    const uint8_t *y1 = y0 + width;
    const uint8_t *up = u_plane + (j >> 1) * cw;
    const uint8_t *vp = v_plane + (j >> 1) * cw;
    uint16_t *d0 = rgb + j * width;
    uint16_t *d1 = d0 + width;
    for (int i = 0; i < width; i += 2) {
      int u = up[i >> 1] - 128;
      int v = vp[i >> 1] - 128;
      int rc = (v * 359) >> 8;
      int gc = (u * 88 + v * 183) >> 8;
      int bc = (u * 454) >> 8;
      for (int k = 0; k < 2; k++) {
        int y = y0[i + k];
        int r = y + rc, g = y - gc, b = y + bc;
        r = r < 0 ? 0 : (r > 255 ? 255 : r);
        g = g < 0 ? 0 : (g > 255 ? 255 : g);
        b = b < 0 ? 0 : (b > 255 ? 255 : b);
        d0[i + k] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        y = y1[i + k];
        r = y + rc, g = y - gc, b = y + bc;
        r = r < 0 ? 0 : (r > 255 ? 255 : r);
        g = g < 0 ? 0 : (g > 255 ? 255 : g);
        b = b < 0 ? 0 : (b > 255 ? 255 : b);
        d1[i + k] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
      }
    }
  }
}

// ============================================================================
// Common Methods
// ============================================================================

void IPCameraViewer::update_canvas_() {
  if (this->canvas_obj_ == nullptr) {
    if (!this->canvas_warning_shown_) {
      ESP_LOGW(TAG, "Canvas not configured");
      this->canvas_warning_shown_ = true;
    }
    return;
  }

  lv_canvas_set_buffer(this->canvas_obj_, this->current_decode_buffer_,
                       this->render_width_(), this->render_height_(), LV_COLOR_FORMAT_RGB565);
  lv_obj_invalidate(this->canvas_obj_);
}

void IPCameraViewer::swap_buffers_() {
  uint8_t *temp = this->current_display_buffer_;
  this->current_display_buffer_ = this->current_decode_buffer_;
  this->current_decode_buffer_ = temp;
}

void IPCameraViewer::configure_canvas(lv_obj_t *canvas) {
  this->canvas_obj_ = canvas;
  ESP_LOGD(TAG, "Canvas configured: %p (will render at %ux%u)", canvas, this->width_, this->height_);
}

void IPCameraViewer::dump_config() {
  ESP_LOGCONFIG(TAG, "IP Camera Viewer:");
  ESP_LOGCONFIG(TAG, "  URL: %s", this->url_.c_str());
  ESP_LOGCONFIG(TAG, "  Protocol: %s", this->protocol_ == Protocol::RTSP ? "RTSP/H264" : "MJPEG");
  ESP_LOGCONFIG(TAG, "  Stream resolution: %ux%u", this->width_, this->height_);
  if (this->resizing_()) {
    ESP_LOGCONFIG(TAG, "  Display resolution: %ux%u (PPA resize)", this->display_width_,
                  this->display_height_);
  }
  ESP_LOGCONFIG(TAG, "  Update interval: %u ms", this->update_interval_);
  ESP_LOGCONFIG(TAG, "  Keep alive: %s", this->keep_alive_ ? "yes (instant redisplay, background "
                                                              "decode stays on while hidden)" : "no");
}

}  // namespace ip_camera_viewer
}  // namespace esphome
