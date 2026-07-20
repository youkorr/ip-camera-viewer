#include "h264_hp_decoder.h"

#include "esphome/core/log.h"
#include "esp_heap_caps.h"

// edge264 (vendorisé sous components/h264_hp/edge264/). Voir README pour le
// sous-module. Tant que la source n'est pas présente, le composant compile en
// mode "no-op" pour ne pas casser le build (USE_H264_HP_EDGE264 non défini).
#if defined(USE_H264_HP_EDGE264)
extern "C" {
// edge264.h est fourni par le composant ESP-IDF "edge264" (h264_hp/edge264/,
// INCLUDE_DIRS "."), enregistré via add_idf_component. Son include public
// remonte ici (le composant esphome REQUIERT les composants idf ajoutés), donc
// l'en-tête se résout dans les deux moteurs de build (PlatformIO et CMake natif).
#include "edge264.h"
}
#endif

namespace esphome {
namespace h264_hp {

static const char *const TAG = "h264_hp";

#if defined(USE_H264_HP_EDGE264)
// --- Callbacks d'allocation : tout en PSRAM -------------------------------
// edge264 demande deux buffers par image : `samples` (plans YCbCr) et `mbs`
// (état macrobloc). Sur P4 ils sont volumineux -> PSRAM obligatoire.
static void psram_alloc_cb(void **samples, unsigned samples_size, void **mbs,
                           unsigned mbs_size, int /*errno_on_fail*/, void * /*arg*/) {
  // edge264's own internal_alloc makes ONE allocation with `mbs` contiguous right
  // after `samples` (*mbs = *samples + samples_size). Some edge264 code relies on
  // that adjacency, so we honour the same contract — two separate allocations can
  // break it. Single 64-byte-aligned PSRAM block, mbs carved from it.
  uint8_t *p = (uint8_t *) heap_caps_aligned_alloc(64, (size_t) samples_size + mbs_size,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  *samples = p;
  *mbs = (p != nullptr) ? p + samples_size : nullptr;
}

static void psram_free_cb(void *samples, void * /*mbs*/, void * /*arg*/) {
  heap_caps_free(samples);  // single block; mbs lives inside it
}

// NOTE: la lib précompilée libedge264.a est bâtie SANS HAS_LOGS. Dans ce cas
// edge264_alloc() fait `if (log_cb) return free(dec), NULL;` -> passer un log_cb
// non-NULL fait ÉCHOUER l'alloc juste après le struct (avant les threads). On
// passe donc nullptr comme log_cb (cf. begin()). Cette fonction reste pour un
// éventuel build de la lib avec logs.
[[maybe_unused]] static int log_cb(const char *str, void * /*arg*/) {
  ESP_LOGV(TAG, "edge264: %s", str);
  return 0;
}

static inline Edge264Decoder *dec_of(void *p) { return static_cast<Edge264Decoder *>(p); }
#endif

H264HpDecoder::~H264HpDecoder() { this->end(); }

bool H264HpDecoder::begin(int n_threads) {
  // Flux caméra live : pas de B-frames, ordre de décodage == ordre d'affichage.
  // Sans ceci, un SPS SANS VUI (Reolink...) fait inférer à edge264 jusqu'à 16
  // frames de réordonnancement à retenir avant la première sortie : secondes de
  // latence et ~1 Mo de PSRAM par slot DPB (écran noir par épuisement mémoire).
  edge264_infer_low_delay = 0;

#if defined(USE_H264_HP_EDGE264)
  if (this->dec_ != nullptr)
    return true;

  // edge264_alloc(n_threads, log_cb, log_arg, log_mbs, alloc_cb, free_cb, alloc_arg)
  // Le struct (~41 Ko) part en PSRAM (CONFIG_SPIRAM_USE_MALLOC + wrap). Mais avec
  // n_threads>0, edge264_alloc crée des threads worker via pthread_create(), dont
  // la pile FreeRTOS doit être en RAM interne : sur ce setup (LVGL + MIPI-DSI +
  // PSRAM en mode malloc) ça échoue et edge264_alloc renvoie NULL. On retombe donc
  // en MONO-THREAD (n_threads=0), qui ne crée aucun thread et renvoie dès que le
  // struct est alloué — décodage plus lent mais fonctionnel (validé sur PC).
  // log_cb = nullptr : la lib précompilée n'a pas HAS_LOGS et REFUSE un log_cb
  // non-NULL (return NULL juste après l'alloc du struct).
  this->dec_ = edge264_alloc(n_threads, nullptr, nullptr, 0, psram_alloc_cb, psram_free_cb, nullptr);
  if (this->dec_ == nullptr && n_threads > 0) {
    ESP_LOGW(TAG,
             "edge264_alloc(%d threads) failed (worker thread creation) — falling "
             "back to single-threaded mode.",
             n_threads);
    this->dec_ = edge264_alloc(0, nullptr, nullptr, 0, psram_alloc_cb, psram_free_cb, nullptr);
    if (this->dec_ != nullptr)
      n_threads = 0;
  }
  if (this->dec_ == nullptr) {
    size_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t in_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGE(TAG, "edge264_alloc failed even single-threaded. Free PSRAM=%u free INTERNAL=%u",
             (unsigned) ps_free, (unsigned) in_free);
    return false;
  }
  ESP_LOGI(TAG, "H.264 High Profile decoder ready (edge264, %d thread(s))", n_threads);
  return true;
#else
  ESP_LOGE(TAG, "edge264 not vendored: add the submodule and -DUSE_H264_HP_EDGE264 (see README)");
  (void) n_threads;
  return false;
#endif
}

void H264HpDecoder::end() {
#if defined(USE_H264_HP_EDGE264)
  if (this->frame_borrowed_)
    this->release_frame();
  if (this->dec_ != nullptr) {
    Edge264Decoder *d = dec_of(this->dec_);
    edge264_free(&d);
    this->dec_ = nullptr;
  }
#endif
}

DecodeStatus H264HpDecoder::decode_nal(const uint8_t *data, size_t len) {
#if defined(USE_H264_HP_EDGE264)
  if (this->dec_ == nullptr)
    return DecodeStatus::NO_DECODER;
  // edge264_decode_NAL attend [buf, end) du *corps* de la NAL.
  int ret = edge264_decode_NAL(dec_of(this->dec_), data, data + len, nullptr, nullptr);
  if (ret < 0) {
    this->decode_errors_++;
    return DecodeStatus::ERROR;
  }
  return DecodeStatus::OK;
#else
  (void) data; (void) len;
  return DecodeStatus::NO_DECODER;
#endif
}

DecodeStatus H264HpDecoder::decode_annexb(const uint8_t *data, size_t len) {
#if defined(USE_H264_HP_EDGE264)
  if (this->dec_ == nullptr)
    return DecodeStatus::NO_DECODER;
  const uint8_t *const end = data + len;

  // Découpe Annex-B DÉTERMINISTE (indépendante de edge264_find_start_code, dont
  // l'offset de retour est ambigu). edge264_decode_NAL lit l'en-tête NAL à buf[0]
  // (il NE saute PAS le start code) : on doit donc lui passer l'octet APRÈS le
  // start code 00 00 01, et non le start code lui-même — sinon nal_unit_type est
  // lu comme 0 (Unknown -> ENOTSUP) et rien n'est décodé.
  auto find_sc = [end](const uint8_t *s) -> const uint8_t * {
    for (const uint8_t *q = s; q + 3 <= end; q++) {
      if (q[0] == 0 && q[1] == 0 && q[2] == 1)
        return q;  // pointe sur le 00 00 01
    }
    return end;
  };

  DecodeStatus last = DecodeStatus::NEED_MORE;
  int n_fed = 0, n_ok = 0;
  const uint8_t *sc = find_sc(data);
  while (sc < end) {
    const uint8_t *nal = sc + 3;  // octet d'en-tête NAL (après 00 00 01)
    if (nal >= end)
      break;
    const uint8_t *nsc = find_sc(nal);  // prochain start code (ou end)
    const uint8_t *nal_end = nsc;
    // Retirer le 0x00 de tête d'un éventuel start code 4 octets (00 00 00 01)
    // qui appartient au start code SUIVANT, pas au corps de la NAL courante.
    while (nal_end > nal && nal_end[-1] == 0)
      nal_end--;

    const uint8_t nut = nal[0] & 0x1f;  // nal_unit_type
    int ret = edge264_decode_NAL(dec_of(this->dec_), nal, nal_end, nullptr, nullptr);
    n_fed++;
    if (ret == 0) {
      n_ok++;
      if (last != DecodeStatus::ERROR)
        last = DecodeStatus::OK;
    } else if (ret < 0 || ret == ENOTSUP || ret == EBADMSG || ret == EINVAL) {
      this->decode_errors_++;
      last = DecodeStatus::ERROR;
    }
    // ENODATA/ENOBUFS/ENOMSG = retours "doux" (fin de flux / DPB plein) : non
    // comptés comme erreurs ; on continue.
    ESP_LOGV(TAG, "NAL type=%u len=%d -> edge264_decode_NAL ret=%d", nut,
             (int) (nal_end - nal), ret);
    sc = nsc;
  }
  ESP_LOGV(TAG, "decode_annexb: %d NAL fournis, %d acceptés (ret=0)", n_fed, n_ok);
  return last;
#else
  (void) data; (void) len;
  return DecodeStatus::NO_DECODER;
#endif
}

bool H264HpDecoder::get_frame(DecodedFrame *out) {
#if defined(USE_H264_HP_EDGE264)
  if (this->dec_ == nullptr || out == nullptr)
    return false;
  if (this->frame_borrowed_)
    this->release_frame();

  Edge264Frame f;
  // borrow=1 : on emprunte les buffers (pas de copie). Rendre via release_frame().
  int ret = edge264_get_frame(dec_of(this->dec_), &f, 1);
  if (ret != 0)
    return false;  // pas de frame disponible

  this->frame_borrowed_ = true;
  // Mémoriser le jeton d'emprunt (1<<pic) : release_frame() DOIT le repasser à
  // edge264_return_frame pour libérer le slot DPB. Sinon output_frames ne se vide
  // jamais et le décodeur tombe en ENOBUFS après ~max_dec_frame_buffering frames.
  this->frame_return_arg_ = f.return_arg;
  this->frames_decoded_++;

  out->y = f.samples[0];
  out->cb = f.samples[1];
  out->cr = f.samples[2];
  // frame_crop_offsets = {top, right, bottom, left} (en pixels). La taille codée
  // (width_Y/height_Y) est alignée macrobloc ; on retire le crop pour la taille
  // d'affichage. Les plans pointent sur l'origine codée ; pour des flux caméra
  // le crop gauche/haut est généralement nul (seuls bas/droite alignent au MB).
  const int crop_top = f.frame_crop_offsets[0];
  const int crop_right = f.frame_crop_offsets[1];
  const int crop_bottom = f.frame_crop_offsets[2];
  const int crop_left = f.frame_crop_offsets[3];
  out->width = f.width_Y - crop_left - crop_right;
  out->height = f.height_Y - crop_top - crop_bottom;
  out->stride_y = f.stride_Y;
  out->stride_c = f.stride_C;
  return true;
#else
  (void) out;
  return false;
#endif
}

void H264HpDecoder::release_frame() {
#if defined(USE_H264_HP_EDGE264)
  if (this->dec_ != nullptr && this->frame_borrowed_) {
    // Passer le return_arg mémorisé (PAS nullptr) : edge264_return_frame fait
    // output_frames &= ~return_arg. Avec nullptr (0) ça ne libère RIEN -> fuite de
    // slots DPB -> ENOBUFS. Avec le bon jeton, le slot est rendu au décodeur.
    edge264_return_frame(dec_of(this->dec_), this->frame_return_arg_);
    this->frame_return_arg_ = nullptr;
    this->frame_borrowed_ = false;
  }
#endif
}

}  // namespace h264_hp
}  // namespace esphome
