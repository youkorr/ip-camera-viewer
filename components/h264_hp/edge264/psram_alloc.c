// PSRAM redirect for edge264's internal struct allocation.
//
// edge264_alloc() allocates its decoder struct (~41 KB) with the libc
// aligned_alloc(), which on ESP-IDF is served from INTERNAL DRAM only. With LVGL
// and the MIPI-DSI display running, internal DRAM is too low/fragmented at that
// point, so the allocation fails -> "edge264_alloc a échoué". (The big per-frame
// YCbCr buffers already go to PSRAM via the wrapper's alloc callback; this is
// only about the one-time struct + any large scratch.)
//
// We wrap aligned_alloc (via -Wl,--wrap=aligned_alloc, added in CMakeLists.txt)
// and route LARGE blocks to PSRAM, leaving small ones (often DMA-capable) in
// internal RAM. ESP-IDF's free() handles PSRAM heap_caps memory transparently,
// so edge264_free()'s plain free() still works.
#include <stdlib.h>
#include <stddef.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

// Provided by the GNU linker because of -Wl,--wrap=aligned_alloc.
extern void *__real_aligned_alloc(size_t alignment, size_t size);

void *__wrap_aligned_alloc(size_t alignment, size_t size) {
  if (size >= 8192) {
    // Blocs MOYENS (la struct décodeur edge264, ~41 Ko) : RAM INTERNE d'abord.
    // Cette struct contient l'état le plus chaud du décodeur — contextes CABAC
    // lus/écrits À CHAQUE BIN, caches de voisinage, état du bitstream. En PSRAM,
    // chaque accès passe par le cache L2 partagé (en concurrence avec les frames
    // du DPB, l'écran DSI et LVGL) ; en SRAM interne c'est 1-2 cycles garantis
    // et ça libère d'autant le L2 pour les données de frame. Le décodage est
    // prouvé memory-bound sur le P4 : c'est le levier principal.
    // Garde-fou : ne prendre l'interne que s'il reste une marge confortable
    // (~160 Ko) après l'allocation — WiFi/LVGL allouent aussi en interne et
    // n'ont pas tous un repli propre. Sinon, PSRAM comme avant.
    static int n_logged = 0;
    if (size <= 100 * 1024) {
      size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (free_int >= size + 160 * 1024) {
        void *p = heap_caps_aligned_alloc(alignment, size,
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p != NULL) {
          if (size >= 20000 && n_logged < 6) {
            n_logged++;
            ESP_LOGI("edge264_psram",
                     "aligned_alloc(%u, %u) -> INTERNAL SRAM (hot decoder state; "
                     "%u KB internal left)",
                     (unsigned) alignment, (unsigned) size,
                     (unsigned) (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
          }
          return p;
        }
      }
    }
    void *p = heap_caps_aligned_alloc(alignment, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // Trace (preuve que le wrap est actif + réponse PSRAM). On logge les gros blocs
    // (~struct edge264) lors des 1res tentatives, donc DANS la fenêtre capturée
    // (après démarrage caméra), pas seulement au boot.
    if (size >= 20000 && n_logged < 6) {
      n_logged++;
      ESP_LOGI("edge264_psram", "aligned_alloc(%u, %u) -> PSRAM %s",
               (unsigned) alignment, (unsigned) size, p ? "OK" : "ÉCHEC");
    }
    if (p != NULL)
      return p;
    // PSRAM exhausted -> fall back to internal DRAM.
  }
  return __real_aligned_alloc(alignment, size);
}
