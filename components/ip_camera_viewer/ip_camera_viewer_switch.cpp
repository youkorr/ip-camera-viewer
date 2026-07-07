#include "ip_camera_viewer_switch.h"
#include "esphome/core/log.h"

namespace esphome {
namespace ip_camera_viewer {

static const char *const TAG = "ip_camera_viewer.switch";

void IPCameraViewerSwitch::setup() {
  // Honor the standard ESPHome `restore_mode` (ALWAYS_ON/ALWAYS_OFF/RESTORE_*)
  // instead of a bespoke preference: the old code always read back the last
  // saved state via its own separate NVS key, so `restore_mode: ALWAYS_OFF`
  // was silently ignored — the camera kept starting enabled (and hammering
  // the network with connection attempts before WiFi was even up) as soon as
  // it had ever been turned on once. get_initial_state_with_restore_mode()
  // is the base Switch class's own logic and already applies ALWAYS_OFF
  // correctly (forces false, never touches the saved preference).
  bool initial_state = this->get_initial_state_with_restore_mode().value_or(false);
  this->write_state(initial_state);
}

void IPCameraViewerSwitch::dump_config() {
  LOG_SWITCH("", "IP Camera Viewer Switch", this);
}

void IPCameraViewerSwitch::write_state(bool state) {
  if (this->camera_ != nullptr) {
    this->camera_->set_enabled(state);
    ESP_LOGI(TAG, "IP Camera Viewer %s", state ? "enabled" : "disabled");
  }

  // publish_state() already persists to its own preference when restore_mode
  // requires it (see Switch::publish_state()) — no separate save needed here.
  this->publish_state(state);
}

}  // namespace ip_camera_viewer
}  // namespace esphome
