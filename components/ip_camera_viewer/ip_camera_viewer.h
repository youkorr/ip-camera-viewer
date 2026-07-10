#pragma once

#include "esphome/core/component.h"
#include "esphome/components/lvgl/lvgl_esphome.h"

#include "esp_http_client.h"
#include "driver/jpeg_decode.h"

// H264 decoder
extern "C" {
#include "esp_h264_dec.h"
#include "esp_h264_dec_sw.h"
}

// H264 High Profile decoder (edge264) — actif seulement si libedge264.a est
// présent (flag défini par le composant h264_hp).
#ifdef USE_H264_HP_EDGE264
#include "esphome/components/h264_hp/h264_hp_decoder.h"
#endif

#include <sys/socket.h>
#include <netinet/in.h>
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

namespace esphome {
namespace ip_camera_viewer {

enum class Protocol {
  MJPEG,
  RTSP,
};

class IPCameraViewer : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_url(const std::string &url) { this->url_ = url; }
  void set_width(uint16_t width) { this->width_ = width; }
  void set_height(uint16_t height) { this->height_ = height; }
  // Taille d'affichage/canvas optionnelle, si différente de la résolution du
  // flux décodé (width_/height_). Étirée EXACTEMENT vers ces dimensions par le
  // PPA matériel (voir ppa_convert_) — pas de préservation automatique du ratio,
  // l'appelant choisit des dimensions cohérentes avec son écran/flux.
  void set_display_size(uint16_t w, uint16_t h) {
    this->display_width_ = w;
    this->display_height_ = h;
    // Demande YAML d'origine, conservée telle quelle : display_width_/height_
    // sont ramenés par init_buffers_ aux dimensions atteignables par le PPA
    // (pas de 1/16) et re-quantifier depuis des valeurs déjà tronquées ferait
    // rétrécir l'image d'un cran à chaque réactivation (effet cliquet).
    this->req_display_width_ = w;
    this->req_display_height_ = h;
  }
  void set_update_interval(uint32_t interval_ms) { this->update_interval_ = interval_ms; }
  void set_enabled(bool enabled) { this->enabled_ = enabled; }
  // Si vrai, le switch OFF n'arrête QUE l'affichage (timer LVGL) : la connexion
  // RTSP/MJPEG et la tâche de décodage continuent en tâche de fond, buffers et
  // décodeur restent alloués. Le switch ON suivant saute donc la reconnexion et
  // la réallocation -> quasi instantané (juste recréer le timer), au prix d'une
  // PSRAM/bande passante caméra utilisées en continu même écran éteint. Défaut
  // false = comportement historique (déconnexion + libération mémoire au OFF).
  void set_keep_alive(bool keep_alive) { this->keep_alive_ = keep_alive; }
  // Active/désactive le keepalive RTSP périodique (GET_PARAMETER toutes les 15 s).
  // Défaut true : nécessaire pour les caméras qui ferment une session de contrôle
  // inactive (Reolink ~30 s). Peut être mis à false pour les caméras qui n'en ont
  // pas besoin (beaucoup de Tapo tiennent la session sans ping) : la réponse RTSP
  // à ce GET_PARAMETER arrive mélangée au flux RTP interleavé et ajoute un peu de
  // bruit à re-synchroniser, donc le couper allège marginalement le pipeline.
  void set_rtsp_keepalive(bool enabled) { this->rtsp_keepalive_enabled_ = enabled; }
  // Profondeur du tampon de lissage (jitter buffer). 0 (défaut) = mode temps réel
  // historique (afficher la dernière frame prête, latence mini, mais hitch visible
  // à chaque IDR). >=2 : la tâche décode d'avance dans un anneau de N frames et le
  // timer d'affichage tire à cadence régulière -> lisse le hitch de l'IDR au prix
  // de ~N frames de latence et N+2 buffers RGB565 de PSRAM. RTSP/H.264 uniquement.
  void set_buffer_frames(uint8_t n) { this->buffer_frames_ = n; }
  void set_protocol(const std::string &protocol) {
    // "rtsp" and "h264" are aliases for the same RTSP/H.264 path
    if (protocol == "rtsp" || protocol == "h264") {
      this->protocol_ = Protocol::RTSP;
    } else {
      this->protocol_ = Protocol::MJPEG;
    }
  }

  void configure_canvas(lv_obj_t *canvas);

  float get_setup_priority() const override { return setup_priority::LATE; }

  // Static callback for LVGL timer
  static void lvgl_timer_callback_(lv_timer_t *timer);

 protected:
  std::string url_{};
  uint16_t width_{640};
  uint16_t height_{480};
  // 0 = pas de redimensionnement demandé (rendu à width_/height_). Utiliser
  // render_width_()/render_height_() plutôt que ces champs directement.
  uint16_t display_width_{0};
  uint16_t display_height_{0};
  uint16_t req_display_width_{0};   // demande YAML (voir set_display_size)
  uint16_t req_display_height_{0};
  uint16_t render_width_() const { return this->display_width_ ? this->display_width_ : this->width_; }
  uint16_t render_height_() const { return this->display_height_ ? this->display_height_ : this->height_; }
  bool resizing_() const { return this->display_width_ != 0; }
  // Facteurs d'échelle PPA quantifiés, en SEIZIÈMES (le SRM du P4 n'accepte que
  // des pas de 1/16, tronqués). Fixés par init_buffers_ : display_width_/height_
  // sont ramenés aux dimensions RÉELLEMENT produites par le PPA, sinon les
  // lignes/colonnes jamais écrites en bord de canvas alternent entre les résidus
  // des deux buffers -> bande scintillante en bas. 16 = échelle 1:1.
  uint16_t ppa_scale16_x_{16};
  uint16_t ppa_scale16_y_{16};
  bool keep_alive_{false};
  Protocol protocol_{Protocol::MJPEG};

  lv_obj_t *canvas_obj_{nullptr};

  uint32_t update_interval_{100};
  uint32_t last_update_{0};

  uint32_t frame_count_{0};
  bool first_update_{true};
  bool canvas_warning_shown_{false};
  bool enabled_{true};

  lv_timer_t *lvgl_timer_{nullptr};

  // WiFi connection retry management
  uint32_t last_connection_attempt_{0};
  uint32_t connection_retry_delay_{15000};  // 15 seconds initial delay
  uint8_t connection_attempts_{0};

  // Network quality adaptation
  uint32_t last_quality_check_{0};
  uint32_t quality_check_interval_{5000};  // Check every 5 seconds
  uint8_t current_quality_level_{1};  // 0=low, 1=medium, 2=high

  // JPEG decoder
  jpeg_decoder_handle_t jpeg_decoder_{nullptr};

  // Double buffer for RGB565 output
  uint8_t *rgb565_buffer_a_{nullptr};
  uint8_t *rgb565_buffer_b_{nullptr};
  uint8_t *current_display_buffer_{nullptr};
  uint8_t *current_decode_buffer_{nullptr};
  size_t rgb565_buffer_size_{0};

  // --- Décodage H.264 sur tâche dédiée (solution "ne pas bloquer LVGL") ------
  // Une I-frame edge264 prend ~11 s sur ce P4 (scalaire). Exécutée sur le
  // loopTask, elle gèle écran/tactile/audio. On la déporte sur une tâche FreeRTOS
  // dédiée : elle fetch+décode+convertit en fond, et le loopTask ne fait plus que
  // POSER l'image (canvas), instantané. Handshake par decode_frame_ready_ :
  // la tâche remplit current_decode_buffer_ puis met ready=true ; le timer affiche,
  // swappe et remet ready=false ; la tâche peut alors préparer la suivante.
  // Si la création de la tâche échoue, decode_task_handle_ reste nullptr et on
  // retombe sur le décodage en ligne (comportement historique).
  TaskHandle_t decode_task_handle_{nullptr};
  // Atomique acquire/release : garantit que les écritures du buffer converti par la
  // tâche sont visibles par le loopTask AVANT qu'il ne voie le flag à true (RISC-V
  // multicœur = mémoire faiblement ordonnée).
  std::atomic<bool> decode_frame_ready_{false};

  // --- Arrêt propre AVEC libération mémoire (switch OFF) ---------------------
  // La tâche de décodage peut être en plein travail (fetch/decode/convert) au
  // moment du stop : libérer les buffers immédiatement = use-after-free.
  // Handshake (seq_cst, pattern Dekker) :
  //  - decode_run_ : consigne "tu peux travailler". Le stop la met à false.
  //  - decode_task_idle_ : la tâche la met à false AVANT de toucher aux buffers
  //    (puis re-vérifie decode_run_), à true quand elle se met en veille.
  //  - pending_shutdown_ : le loop() re-tente à chaque tour ; dès que la tâche
  //    est idle, il déconnecte et libère (buffers + DPB edge264, plusieurs Mo).
  std::atomic<bool> decode_run_{true};
  std::atomic<bool> decode_task_idle_{true};
  bool pending_shutdown_{false};

  // --- Tampon de lissage (jitter buffer) — voir set_buffer_frames() -----------
  // Anneau de N+2 buffers RGB565 échangés entre la tâche de décodage (producteur,
  // cœur 1) et le timer d'affichage (consommateur, cœur 0) via deux files
  // FreeRTOS (thread-safe SMP). Le producteur convertit chaque frame décodée dans
  // un buffer LIBRE et le pousse dans "filled" ; s'il n'y a plus de buffer libre,
  // il évince la plus ANCIENNE frame de "filled" pour la réutiliser (latence
  // bornée à N, on garde toujours les frames les plus fraîches). Le consommateur
  // tire la plus ancienne de "filled" à chaque tick régulier -> cadence lisse.
  // Actif seulement si buffer_frames_>0 ET la tâche dédiée existe (voir ring_active_()).
  static constexpr uint8_t IPCV_RING_MAX = 5;  // N max = 3 -> cap max = 5
  uint8_t buffer_frames_{0};                    // N (0 = désactivé)
  uint8_t ring_cap_{0};                         // N+2 quand alloué, sinon 0
  uint8_t *ring_buf_[IPCV_RING_MAX]{};          // buffers de l'anneau (alloués à part de a_/b_)
  uint8_t *ring_display_buf_{nullptr};          // buffer actuellement affiché (détenu par le conso)
  QueueHandle_t ring_free_q_{nullptr};          // buffers libres (pointeurs)
  QueueHandle_t ring_filled_q_{nullptr};        // buffers prêts à afficher (FIFO)
  bool ring_active_() const {
    return this->ring_cap_ > 0 && this->decode_task_handle_ != nullptr;
  }
  bool init_frame_ring_();   // alloue l'anneau + les files (si buffer_frames_>0)
  void free_frame_ring_();   // libère l'anneau + les files
  bool ring_show_next_();    // consommateur : affiche la prochaine frame prête (true si affichée)
  void set_canvas_buffer_(uint8_t *buf);  // pose un buffer sur le canvas (dims de rendu)
  // Convertit la frame décodée (yuv_buffer_/DPB) vers dst en RGB565 (PPA matériel
  // ou repli scalaire), avec le chrono de diagnostic. Renvoie true si convertie
  // (false = frame sautée, ex. resize impossible sur le chemin scalaire).
  bool convert_decoded_(uint8_t *dst);

  // --- Conversion YUV->RGB565 matérielle (PPA du P4) --------------------------
  // La conversion scalaire coûte ~40 ms/frame (dominée par le trafic PSRAM, en
  // concurrence avec l'écran MIPI-DSI) et plafonne l'affichage à ~6-7 fps. Le
  // bloc PPA sait convertir YUV420->RGB565 en DMA. Son format YUV420 matériel
  // est packé par lignes (lignes paires "U Y Y U Y Y...", impaires "V Y Y...",
  // cf. ESP_H264_RAW_FMT_O_UYY_E_VYY) : le repack depuis les plans I420 du DPB
  // edge264 est FUSIONNÉ dans la copie de sortie du décodeur (coût ~équivalent
  // aux memcpy qu'il remplace). Repli scalaire automatique si l'init PPA échoue,
  // si les dimensions flux != config (letterbox), ou si une opération échoue.
  void *ppa_client_{nullptr};   // ppa_client_handle_t (opaque pour l'en-tête)
  uint8_t *ouyy_buffer_{nullptr};
  bool ppa_ok_{false};
  bool yuv_is_ouyy_{false};  // la frame en attente est au format O_UYY_E_VYY
  void init_ppa_();
  bool ppa_convert_(uint8_t *dst_rgb565);

  // JPEG receive buffer
  uint8_t *jpeg_buffer_{nullptr};
  size_t jpeg_buffer_size_{0};
  size_t jpeg_data_len_{0};

  // MJPEG parse buffer (dynamically allocated in PSRAM to save SRAM)
  uint8_t *parse_buffer_{nullptr};
  size_t parse_buffer_size_{0};
  size_t parse_buffer_len_{0};

  // HTTP client (MJPEG)
  esp_http_client_handle_t http_client_{nullptr};
  bool stream_connected_{false};
  uint32_t stream_connect_time_{0};  // Time when stream was connected
  uint32_t stream_reconnect_interval_{180000};  // Reconnect every 3 minutes (180 seconds)

  // MJPEG parsing state
  enum class MjpegState {
    SEARCHING_BOUNDARY,
    READING_HEADERS,
    READING_CONTENT,
  };
  MjpegState mjpeg_state_{MjpegState::SEARCHING_BOUNDARY};
  size_t content_length_{0};

  // RTSP client
  int rtsp_socket_{-1};
  int rtp_socket_{-1};
  uint16_t rtp_port_{0};
  std::string rtsp_session_{};
  // URL de contrôle du média vidéo (issue du SDP), mémorisée pour l'URI des
  // requêtes de keepalive périodiques (GET_PARAMETER) pendant le streaming.
  std::string rtsp_control_url_{};
  // Dernier keepalive RTSP envoyé (millis). La caméra ferme une session inactive
  // (~30 s sur Reolink) : sans "ping" périodique, le flux s'arrête au bout de ~30 s
  // même si le socket TCP reste ouvert (symptôme : eagain permanent, 0 nouveau
  // paquet). On envoie un GET_PARAMETER toutes les 15 s pour garder la session.
  uint32_t last_keepalive_{0};
  bool rtsp_keepalive_enabled_{true};  // voir set_rtsp_keepalive()
  // Watchdog de famine RTP : millis() du dernier octet reçu. Une session peut
  // mourir côté caméra SANS erreur socket (recv() -> EAGAIN pour toujours,
  // compteurs figés) ; après 10 s sans le moindre octet on ferme et on marque
  // le flux déconnecté pour que loop() se reconnecte (y compris en plein
  // affichage — voir le bloc de reconnexion mi-session dans loop()).
  uint32_t last_rx_data_{0};
  std::string rtsp_auth_{};  // Base64 encoded credentials (Basic auth)
  std::string rtsp_user_{};  // Username (for Digest auth)
  std::string rtsp_pass_{};  // Password (for Digest auth)
  // Digest auth challenge (parsed from a 401 WWW-Authenticate response)
  std::string digest_realm_{};
  std::string digest_nonce_{};
  std::string digest_qop_{};
  std::string digest_opaque_{};
  uint32_t digest_nc_{0};
  int cseq_{1};

  // H264 decoder
  esp_h264_dec_handle_t h264_decoder_{nullptr};

#ifdef USE_H264_HP_EDGE264
  // Décodeur High Profile (edge264) : utilisé quand le flux n'est pas Baseline.
  h264_hp::H264HpDecoder hp_decoder_;
  bool use_hp_decoder_{false};
  bool hp_started_{false};
#endif

  // H264 receive buffer
  uint8_t *h264_buffer_{nullptr};
  size_t h264_buffer_size_{0};
  size_t h264_data_len_{0};

  // Persistent RTP-over-TCP reassembly buffer. The interleaved '$' frames split
  // arbitrarily across recv()s on a non-blocking socket; we accumulate raw bytes
  // here and parse only COMPLETE '$'-framed packets, never consuming a header
  // before its payload has fully arrived (avoids permanent framing desync).
  // Sized well above one interleaved packet (4 + RTP<=1500).
  uint8_t rtp_acc_[8192]{0};
  size_t rtp_acc_len_{0};

  // Régulateur de latence : quand le décodage ne suit plus le débit de la
  // caméra (P-frames chargées : bruit IR nocturne, mouvement), le backlog TCP
  // grossit sans limite -> flux "au ralenti" avec un retard qui s'accumule.
  // Détection : socket saturé (FIONREAD + accumulateur > seuil) plusieurs
  // contrôles de suite -> on jette les P-frames SANS les décoder jusqu'à la
  // prochaine IDR (impossible de sauter une P isolée, elles se référencent en
  // chaîne), puis reprise en direct. Latence bornée à ~1 GOP au pire.
  bool catchup_skip_to_idr_{false};
  uint32_t catchup_last_check_{0};
  uint8_t catchup_full_ticks_{0};

  // H264 SPS/PPS caching (required for proper decoder initialization)
  uint8_t sps_cache_[128]{0};  // SPS cache (typically < 50 bytes)
  size_t sps_len_{0};
  uint8_t pps_cache_[64]{0};   // PPS cache (typically < 20 bytes)
  size_t pps_len_{0};
  bool has_sps_{false};
  bool has_pps_{false};
  bool param_sets_sent_{false};      // Track if SPS/PPS sent for normal NAL units
  bool param_sets_sent_fua_{false};  // Track if SPS/PPS sent for fragmented units

  // YUV buffer for H264 output
  uint8_t *yuv_buffer_{nullptr};
  size_t yuv_buffer_size_{0};

  // Common methods
  bool init_buffers_();
  void free_buffers_();  // Free PSRAM buffers when camera is disabled
  bool init_jpeg_decoder_();
  void update_canvas_();
  void swap_buffers_();
  void check_network_quality_();  // Monitor network conditions
  void adapt_to_network_();

  // MJPEG methods
  bool connect_mjpeg_stream_();
  void disconnect_mjpeg_stream_();
  bool fetch_jpeg_frame_();
  size_t strip_jpeg_com_markers_(uint8_t *data, size_t len);  // Strip COM markers from JPEG
  size_t strip_jpeg_restart_markers_(uint8_t *data, size_t len);  // Strip RST markers (unsupported by hardware)
  bool decode_jpeg_to_rgb565_();

  // RTSP methods
  bool connect_rtsp_stream_();
  void disconnect_rtsp_stream_();
  bool init_h264_decoder_();
  bool send_rtsp_request_(const std::string &method, const std::string &url, const std::string &extra_headers = "", std::string *response_body = nullptr);
  // Keepalive "fire-and-forget" : envoie un GET_PARAMETER sur le socket RTSP sans
  // lire la réponse (elle arrive entrelacée aux paquets '$' et est jetée par la
  // resync du lecteur RTP). Empêche l'expiration de la session pendant le stream.
  void send_rtsp_keepalive_();
  std::string build_rtsp_auth_header_(const std::string &method, const std::string &uri);  // Basic or Digest
  bool parse_rtsp_response_(std::string &response);
  bool fetch_rtp_frame_();
  bool decode_h264_to_yuv_();
  void convert_yuv420_to_rgb565_(uint8_t *yuv, uint8_t *rgb565, int width, int height);

  // Boucle de la tâche de décodage dédiée (voir decode_task_handle_).
  static void decode_task_fn_(void *arg);
};

}  // namespace ip_camera_viewer
}  // namespace esphome
