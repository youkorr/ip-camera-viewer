#pragma once

#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "ip_camera_viewer.h"

namespace esphome {
namespace ip_camera_viewer {

class IPCameraViewerSwitch : public switch_::Switch, public Component {
 public:
  void setup() override;
  void dump_config() override;

  void set_camera(IPCameraViewer *camera) { this->camera_ = camera; }

 protected:
  void write_state(bool state) override;

  IPCameraViewer *camera_{nullptr};
  ESPPreferenceObject rtc_;
};

}  // namespace ip_camera_viewer
}  // namespace esphome
