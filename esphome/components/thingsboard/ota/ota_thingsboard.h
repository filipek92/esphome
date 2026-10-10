#pragma once

#include "esphome/components/http_request/http_request.h"
#include "esphome/components/ota/ota_backend_factory.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#include <string>

#include "../thingsboard.h"

namespace esphome {
namespace thingsboard {

enum OtaThingsBoardError : uint8_t {
  OTA_TB_MD5_INVALID = 0x10,
  OTA_TB_BAD_URL = 0x11,
  OTA_TB_CONNECTION_ERROR = 0x12,
  OTA_TB_NO_UPDATE = 0x13,
};

class OtaThingsBoardComponent : public ota::OTAComponent, public Parented<http_request::HttpRequestComponent> {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_bridge(ThingsBoardBridge *bridge) { this->bridge_ = bridge; }

  /// Called by ThingsBoardBridge when fw_* shared attributes arrive.
  void on_firmware_attributes(const std::string &title, const std::string &version, const std::string &checksum,
                              const std::string &checksum_algorithm);

 protected:
  uint8_t do_ota_(const std::string &url, const std::string &md5);
  void send_fw_state_(const std::string &state, const std::string &error = "");
  void send_fw_telemetry_();

  ThingsBoardBridge *bridge_{nullptr};

  std::string fw_title_{};
  std::string fw_version_{};
  std::string fw_checksum_{};
  std::string fw_checksum_algorithm_{};

  static const uint16_t HTTP_RECV_BUFFER = 256;
};

}  // namespace thingsboard
}  // namespace esphome
