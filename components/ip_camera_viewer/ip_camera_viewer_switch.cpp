#include "ip_camera_viewer_switch.h"
#include "esphome/core/log.h"

namespace esphome {
namespace ip_camera_viewer {

static const char *const TAG = "ip_camera_viewer.switch";

void IPCameraViewerSwitch::setup() {
  // Restore previous state or default to OFF
  bool initial_state = false;
  this->rtc_ = global_preferences->make_preference<bool>(this->get_object_id_hash());
  if (this->rtc_.load(&initial_state)) {
    this->write_state(initial_state);
  } else {
    this->write_state(false);
  }
}

void IPCameraViewerSwitch::dump_config() {
  LOG_SWITCH("", "IP Camera Viewer Switch", this);
}

void IPCameraViewerSwitch::write_state(bool state) {
  if (this->camera_ != nullptr) {
    this->camera_->set_enabled(state);
    ESP_LOGI(TAG, "IP Camera Viewer %s", state ? "enabled" : "disabled");
  }

  // Save state to RTC
  this->rtc_.save(&state);

  // Publish the new state
  this->publish_state(state);
}

}  // namespace ip_camera_viewer
}  // namespace esphome
