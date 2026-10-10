#include "ota_thingsboard.h"

#include "esphome/components/md5/md5.h"
#include "esphome/components/watchdog/watchdog.h"
#include "esphome/core/application.h"
#include "esphome/core/defines.h"
#include "esphome/core/log.h"

namespace esphome {
namespace thingsboard {

static const char *const TAG = "thingsboard.ota";

void OtaThingsBoardComponent::setup() {
  if (this->bridge_ == nullptr) {
    ESP_LOGE(TAG, "ThingsBoard bridge not set");
    this->mark_failed();
    return;
  }
  this->bridge_->set_ota_component(this);
}

void OtaThingsBoardComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "ThingsBoard OTA:");
  ESP_LOGCONFIG(TAG, "  Server: %s", this->bridge_ ? this->bridge_->get_server().c_str() : "not set");
}

void OtaThingsBoardComponent::on_firmware_attributes(const std::string &title, const std::string &version,
                                                     const std::string &checksum,
                                                     const std::string &checksum_algorithm) {
  ESP_LOGI(TAG, "Firmware update available: %s v%s", title.c_str(), version.c_str());

  // Check if this is actually a new firmware
  if (title == this->fw_title_ && version == this->fw_version_) {
    ESP_LOGD(TAG, "Firmware already up-to-date, ignoring");
    return;
  }

  this->fw_title_ = title;
  this->fw_version_ = version;
  this->fw_checksum_ = checksum;
  this->fw_checksum_algorithm_ = checksum_algorithm;

  // Build the firmware download URL:
  // https://server/api/v1/{token}/firmware?title={title}&version={version}
  bool use_ssl = (this->bridge_->get_port() == 8883 || this->bridge_->get_port() == 443);
  std::string url = (use_ssl ? "https://" : "http://") + this->bridge_->get_server() + "/api/v1/" +
                    this->bridge_->get_token() + "/firmware?title=" + title + "&version=" + version;

  ESP_LOGI(TAG, "Starting OTA from: %s", url.c_str());

  // Report DOWNLOADING state
  this->send_fw_state_("DOWNLOADING");

#ifdef USE_OTA_STATE_LISTENER
  this->notify_state_(ota::OTA_STARTED, 0.0f, 0);
#endif

  // MD5: ThingsBoard may use different checksum algorithms, but ESPHome OTA backend uses MD5
  std::string md5;
  if (checksum_algorithm == "MD5" || checksum_algorithm == "md5") {
    md5 = checksum;
  }
  // If checksum algorithm is not MD5, we skip MD5 verification and rely on OTA backend

  auto ota_status = this->do_ota_(url, md5);

  switch (ota_status) {
    case ota::OTA_RESPONSE_OK:
      this->send_fw_state_("UPDATED");
      this->send_fw_telemetry_();
#ifdef USE_OTA_STATE_LISTENER
      this->notify_state_(ota::OTA_COMPLETED, 100.0f, ota_status);
#endif
      delay(10);
      App.safe_reboot();
      break;

    default: {
      ESP_LOGE(TAG, "OTA failed with error code: 0x%02X", ota_status);
      char error[24];
      snprintf(error, sizeof(error), "OTA error code: %u", ota_status);
      this->send_fw_state_("FAILED", error);
#ifdef USE_OTA_STATE_LISTENER
      this->notify_state_(ota::OTA_ERROR, 0.0f, ota_status);
#endif
      break;
    }
  }
}

uint8_t OtaThingsBoardComponent::do_ota_(const std::string &url, const std::string &md5) {
  uint8_t buf[HTTP_RECV_BUFFER + 1];
  uint32_t last_progress = 0;
  uint32_t update_start_time = millis();
  md5::MD5Digest md5_receive;
  char md5_receive_str[33];

  ESP_LOGI(TAG, "Connecting to: %s", url.c_str());

  auto container = this->parent_->get(url);

  if (container == nullptr || container->status_code != http_request::HTTP_STATUS_OK) {
    ESP_LOGE(TAG, "HTTP GET failed, status: %d", container ? container->status_code : -1);
    return OTA_TB_CONNECTION_ERROR;
  }

  ESP_LOGD(TAG, "Firmware size: %u bytes", container->content_length);

  // Initialize MD5 computation
  md5_receive.init();

  auto backend = ota::make_ota_backend();
  auto error_code = backend->begin(container->content_length);
  if (error_code != ota::OTA_RESPONSE_OK) {
    ESP_LOGW(TAG, "backend->begin error: %d", error_code);
    backend->abort();
    container->end();
    return error_code;
  }

  this->send_fw_state_("DOWNLOADING");

  uint32_t last_data_time = millis();
  const uint32_t read_timeout = this->parent_->get_timeout();
  bool update_started = false;

  while (container->get_bytes_read() < container->content_length) {
    int bufsize_or_error = container->read(buf, HTTP_RECV_BUFFER);

    App.feed_wdt();
    yield();

    auto result = http_request::http_read_loop_result(bufsize_or_error, last_data_time, read_timeout,
                                                      container->is_read_complete());
    if (result == http_request::HttpReadLoopResult::RETRY)
      continue;
    if (result == http_request::HttpReadLoopResult::COMPLETE)
      break;
    if (result != http_request::HttpReadLoopResult::DATA) {
      if (result == http_request::HttpReadLoopResult::TIMEOUT) {
        ESP_LOGE(TAG, "Timeout reading data");
      } else {
        ESP_LOGE(TAG, "Error reading data: %d", bufsize_or_error);
      }
      if (update_started)
        backend->abort();
      container->end();
      return OTA_TB_CONNECTION_ERROR;
    }

    if (bufsize_or_error <= HTTP_RECV_BUFFER) {
      md5_receive.add(buf, bufsize_or_error);

      update_started = true;
      error_code = backend->write(buf, bufsize_or_error);
      if (error_code != ota::OTA_RESPONSE_OK) {
        ESP_LOGE(TAG, "Error writing to flash at offset %d", container->get_bytes_read() - bufsize_or_error);
        backend->abort();
        container->end();
        return error_code;
      }
    }

    uint32_t now = millis();
    if ((now - last_progress > 1000) || (container->get_bytes_read() == container->content_length)) {
      last_progress = now;
      float percentage = container->get_bytes_read() * 100.0f / container->content_length;
      ESP_LOGD(TAG, "Progress: %0.1f%%", percentage);
#ifdef USE_OTA_STATE_LISTENER
      this->notify_state_(ota::OTA_IN_PROGRESS, percentage, 0);
#endif
    }
  }

  ESP_LOGI(TAG, "Download done in %.0f seconds", float(millis() - update_start_time) / 1000);

  this->send_fw_state_("DOWNLOADED");

  // Verify MD5 if we have an expected value
  md5_receive.calculate();
  md5_receive.get_hex(md5_receive_str);

  if (!md5.empty()) {
    if (strncmp(md5_receive_str, md5.c_str(), 32) != 0) {
      ESP_LOGE(TAG, "MD5 mismatch: expected %s, got %s", md5.c_str(), md5_receive_str);
      backend->abort();
      container->end();
      return ota::OTA_RESPONSE_ERROR_MD5_MISMATCH;
    }
  }

  backend->set_update_md5(md5_receive_str);

  this->send_fw_state_("VERIFIED");

  container->end();

  App.feed_wdt();
  yield();
  delay(100);  // NOLINT

  this->send_fw_state_("UPDATING");

  error_code = backend->end();
  if (error_code != ota::OTA_RESPONSE_OK) {
    ESP_LOGW(TAG, "Error ending update! error_code: %d", error_code);
    return error_code;
  }

  ESP_LOGI(TAG, "Update complete");
  return ota::OTA_RESPONSE_OK;
}

void OtaThingsBoardComponent::send_fw_state_(const std::string &state, const std::string &error) {
  if (this->bridge_ == nullptr)
    return;

  if (error.empty()) {
    this->bridge_->send_telemetry("fw_state", state);
  } else {
    this->bridge_->send_telemetry("fw_state", state);
    this->bridge_->send_telemetry("fw_error", error);
  }
}

void OtaThingsBoardComponent::send_fw_telemetry_() {
  if (this->bridge_ == nullptr)
    return;

  this->bridge_->send_telemetry("current_fw_title", this->fw_title_);
  this->bridge_->send_telemetry("current_fw_version", this->fw_version_);
}

}  // namespace thingsboard
}  // namespace esphome
