import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_URL

DEPENDENCIES = ["wifi"]
# Auto-chargés pour que l'utilisateur n'ait à mettre que [ip_camera_viewer] :
#  - esp_h264 : fournit les en-têtes du décodeur (sinon esp_h264_dec.h introuvable
#    car ESPHome exécute __init__.py depuis l'arbre copié, sans esp_h264).
#  - h264_hp  : décodeur High Profile edge264 (libedge264.a).
AUTO_LOAD = ["esp_h264", "h264_hp"]
CODEOWNERS = ["@youkorr"]

CONF_CANVAS_ID = "canvas_id"
CONF_UPDATE_INTERVAL = "update_interval"
CONF_WIDTH = "width"
CONF_HEIGHT = "height"
CONF_DISPLAY_WIDTH = "display_width"
CONF_DISPLAY_HEIGHT = "display_height"
CONF_KEEP_ALIVE = "keep_alive"
CONF_RTSP_KEEPALIVE = "rtsp_keepalive"
CONF_BUFFER_FRAMES = "buffer_frames"
CONF_PROTOCOL = "protocol"

ip_camera_viewer_ns = cg.esphome_ns.namespace("ip_camera_viewer")
IPCameraViewer = ip_camera_viewer_ns.class_("IPCameraViewer", cg.Component)

def _validate_url_protocol(config):
    """Catch the common url/protocol mismatch at compile time."""
    url = config[CONF_URL]
    proto = config[CONF_PROTOCOL]
    scheme = url.split("://", 1)[0].lower() if "://" in url else ""
    if scheme == "rtsp" and proto == "mjpeg":
        raise cv.Invalid(
            "An 'rtsp://' URL requires 'protocol: rtsp' (or 'h264'). "
            "'protocol: mjpeg' is only for http:// MJPEG streams (e.g. via go2rtc)."
        )
    if scheme in ("http", "https") and proto in ("rtsp", "h264"):
        raise cv.Invalid(
            "An 'http(s)://' URL is not valid with 'protocol: rtsp/h264'. "
            "Use 'protocol: mjpeg' for http MJPEG streams, or an 'rtsp://' URL for RTSP."
        )
    if (CONF_DISPLAY_WIDTH in config) != (CONF_DISPLAY_HEIGHT in config):
        raise cv.Invalid(
            "'display_width' and 'display_height' must be set together (or neither, "
            "to display at the stream's native width/height)."
        )
    if CONF_DISPLAY_WIDTH in config and proto == "mjpeg":
        raise cv.Invalid(
            "'display_width'/'display_height' need the ESP32-P4 PPA path, only wired "
            "for protocol: rtsp/h264. The MJPEG path is decoded directly at native "
            "size by the hardware JPEG decoder."
        )
    if config.get(CONF_BUFFER_FRAMES, 0) and proto == "mjpeg":
        raise cv.Invalid(
            "'buffer_frames' (smoothing jitter buffer) is only wired for the "
            "protocol: rtsp/h264 decode task. MJPEG is decoded inline at display "
            "rate and doesn't use it."
        )
    return config


# Single camera schema
IP_CAMERA_VIEWER_SCHEMA = cv.All(cv.Schema({
    cv.GenerateID(): cv.declare_id(IPCameraViewer),
    cv.Required(CONF_URL): cv.string,
    cv.Required(CONF_CANVAS_ID): cv.string,
    cv.Required(CONF_WIDTH): cv.int_range(min=16, max=1920),
    cv.Required(CONF_HEIGHT): cv.int_range(min=16, max=1080),
    # Optionnel : taille d'affichage/canvas si différente de width/height (la
    # résolution du FLUX décodé). Utile quand le flux caméra (souvent bas, ex.
    # 640x360 pour un sous-flux Reolink/Tapo) doit remplir un écran plus grand
    # (Waveshare/Guition, résolutions variées selon le projet). Agrandi par le
    # PPA matériel du P4 dans la même passe que la conversion YUV->RGB565 (quasi
    # gratuit). Étire EXACTEMENT vers ces dimensions : à l'utilisateur de choisir
    # un ratio cohérent avec son écran/flux si l'aspect doit être préservé — le
    # composant ne déforme pas "pour vous", il fait ce qui est demandé.
    cv.Optional(CONF_DISPLAY_WIDTH): cv.int_range(min=16, max=1920),
    cv.Optional(CONF_DISPLAY_HEIGHT): cv.int_range(min=16, max=1080),
    cv.Optional(CONF_PROTOCOL, default="mjpeg"): cv.one_of("mjpeg", "rtsp", "h264", lower=True),
    # Par défaut (false) : le switch OFF déconnecte le flux et libère la PSRAM
    # (décodeur inclus) — comportement historique, sobre en ressources. Activer
    # keep_alive fait tourner la connexion + le décodage en tâche de fond même
    # switch éteint, pour que le ON suivant n'ait qu'à réafficher (quasi
    # instantané) au lieu de reconnecter + réallouer (~1-2 s à froid, RTSP High
    # Profile) — utile pour un affichage déclenché par automatisation qui doit
    # réagir vite. Coûte de la PSRAM et de la bande passante caméra en continu.
    cv.Optional(CONF_KEEP_ALIVE, default=False): cv.boolean,
    # Keepalive RTSP (GET_PARAMETER toutes les 15 s). Défaut true : certaines
    # caméras (Reolink ~30 s) ferment une session de contrôle inactive sans ce
    # "ping" et le flux se fige. À mettre false pour les caméras qui tiennent la
    # session seules (souvent le cas des Tapo) : la réponse RTSP au GET_PARAMETER
    # arrive mêlée au flux RTP interleavé, la couper allège un peu le pipeline.
    cv.Optional(CONF_RTSP_KEEPALIVE, default=True): cv.boolean,
    # Tampon de lissage (jitter buffer). 0 (défaut) = temps réel, latence mini
    # mais un léger à-coup à chaque image-clé (IDR). 2-3 = la tâche décode
    # d'avance et l'affichage tire à cadence régulière -> vidéo lissée, au prix
    # de ~N frames de latence et N+2 buffers RGB565 en PSRAM. RTSP/H.264 seul.
    # Utile surtout si la source est proche de la limite de décodage du P4.
    cv.Optional(CONF_BUFFER_FRAMES, default=0): cv.All(
        cv.int_, cv.one_of(0, 2, 3)
    ),
    cv.Optional(CONF_UPDATE_INTERVAL, default="100ms"): cv.positive_time_period_milliseconds,
}).extend(cv.COMPONENT_SCHEMA), _validate_url_protocol)

# Support multiple cameras as a list
CONFIG_SCHEMA = cv.All(
    cv.ensure_list(IP_CAMERA_VIEWER_SCHEMA),
)


async def to_code(config):
    # ESP32-P4 specific build flags for hardware decoders (once for all cameras)
    # Note: H264/JPEG decoders are built-in ESP-IDF components for ESP32-P4
    cg.add_build_flag("-DCONFIG_IDF_TARGET_ESP32P4=1")
    cg.add_build_flag("-DCONFIG_JPEG_ENABLE_DEBUG_LOG=0")
    cg.add_build_flag("-DCONFIG_ESP_H264_DECODER=1")

    # Les en-têtes et libs H.264 sont fournis par des COMPOSANTS ESP-IDF :
    #   - esp_h264 (esp_h264/CMakeLists.txt)        -> esp_h264_dec.h, tinyh264, openh264
    #   - edge264  (h264_hp/edge264/CMakeLists.txt) -> edge264.h, libedge264.a
    # tous deux enregistrés via add_idf_component (voir esp_h264/__init__.py et
    # h264_hp/__init__.py, AUTO_LOAD). Plus de -I bricolé ni d'extra_script
    # PlatformIO : ça marche avec le moteur PlatformIO ET le moteur CMake/Ninja
    # natif d'ESPHome (2026.7+), qui n'exécute pas les extra_scripts PlatformIO.

    # Process each camera in the list
    for cam_config in config:
        var = cg.new_Pvariable(cam_config[CONF_ID])
        await cg.register_component(var, cam_config)

        cg.add(var.set_url(cam_config[CONF_URL]))
        cg.add(var.set_width(cam_config[CONF_WIDTH]))
        cg.add(var.set_height(cam_config[CONF_HEIGHT]))
        if CONF_DISPLAY_WIDTH in cam_config:
            cg.add(var.set_display_size(cam_config[CONF_DISPLAY_WIDTH], cam_config[CONF_DISPLAY_HEIGHT]))
        cg.add(var.set_protocol(cam_config[CONF_PROTOCOL]))
        cg.add(var.set_keep_alive(cam_config[CONF_KEEP_ALIVE]))
        cg.add(var.set_rtsp_keepalive(cam_config[CONF_RTSP_KEEPALIVE]))
        cg.add(var.set_buffer_frames(cam_config[CONF_BUFFER_FRAMES]))

        update_interval_ms = cam_config[CONF_UPDATE_INTERVAL].total_milliseconds
        cg.add(var.set_update_interval(int(update_interval_ms)))
