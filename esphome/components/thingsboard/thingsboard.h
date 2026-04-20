#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/defines.h"
#include "esphome/core/log.h"
#include <ArduinoJson.h>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>
#include <mutex>

// --- PODMÍNĚNÉ IMPORTY ---
#ifdef USE_ARDUINO
  #include <PubSubClient.h>
  #include <WiFiClientSecure.h>
#elif defined(USE_ESP_IDF)
  #include <mqtt_client.h>
#endif

#ifdef USE_LOGGER
#include "esphome/components/logger/logger.h"
#endif

namespace esphome {
namespace thingsboard {

class ThingsBoardBridge;
extern ThingsBoardBridge *global_tb_bridge;

#ifdef USE_ARDUINO
  void tb_mqtt_callback(char* topic, byte* payload, unsigned int length);
#endif

inline std::string tb_get_id(EntityBase *obj) {
  char buf[OBJECT_ID_MAX_LEN];
  return std::string(obj->get_object_id_to(std::span<char, OBJECT_ID_MAX_LEN>(buf)));
}

struct LightStateEntry {
  std::string id;
  bool is_on;
};

class ThingsBoardBridge : public Component
{
 public:
  ThingsBoardBridge(const std::string& server, uint16_t port, const std::string& token);

  void setup() override;
  void loop() override;
  void dump_config() override;
  
  void process_rpc(std::string topic, std::string payload);
  void process_shared_attributes(const std::string &payload);
  void send_device_attributes();
  void send_initial_state();
  void send_telemetry(std::string id, float value);
  void send_telemetry(std::string id, bool value);
  void send_telemetry(std::string id, std::string value);
  void send_attribute(std::string id, std::string value);

  Trigger<std::string, std::string> *get_rpc_trigger() { return &this->rpc_trigger_; }
  Trigger<std::string, std::string> *get_attribute_trigger() { return &this->attribute_trigger_; }
  void set_rpc_response(const std::string &response) { this->rpc_response_ = response; }
  void set_auto_telemetry(bool auto_telemetry) { this->auto_telemetry_ = auto_telemetry; }

  template<typename TGlobal> void add_global_attribute(const std::string &key, TGlobal *global) {
    this->global_attributes_.push_back({
        key,
        [global]() { return ThingsBoardBridge::global_value_to_string_(id(global)); },
        [global](const std::string &value_str) { 
          return ThingsBoardBridge::update_global_from_json_<std::decay_t<decltype(id(global))>>(id(global), value_str); 
        },
    });
  }

  // Overloads for attribute sending from lambdas
  void send_attribute(std::string id, float value);
  void send_attribute(std::string id, bool value);

#ifdef USE_LOG_LISTENERS
  void set_log_level(uint8_t level) { this->log_level_ = level; }
  void on_log(uint8_t level, const char *tag, const char *message, size_t message_len);
#endif

  // --- SPECIFICKÉ METODY PRO ESP-IDF ---
#ifdef USE_ESP_IDF
  esp_mqtt_client_handle_t get_client() { return this->mqtt_client_; }

  struct MqttEvent {
    int event_id;
    std::string topic;
    std::string data;
  };
  std::mutex event_mutex_;
  std::vector<MqttEvent> pending_events_;
#elif defined(USE_ARDUINO)
  bool reconnect();
#endif

 protected:
  struct GlobalAttributeEntry {
    std::string key;
    std::function<std::string()> value_getter;
    std::function<bool(const std::string &)> value_setter;  // Returns true if successfully updated
  };

  template<typename T> static std::string global_value_to_string_(const T &value) {
    using value_t = std::decay_t<T>;
    if constexpr (std::is_same_v<value_t, std::string>) {
      return value;
    } else if constexpr (std::is_same_v<value_t, bool>) {
      return value ? "true" : "false";
    } else if constexpr (std::is_floating_point_v<value_t>) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%.6f", static_cast<double>(value));
      return std::string(buf);
    } else if constexpr (std::is_integral_v<value_t>) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
      return std::string(buf);
    } else {
      return "<unsupported_type>";
    }
  }

  template<typename T> static bool update_global_from_json_(T &global, const std::string &json_value_str) {
    using value_t = std::decay_t<T>;
    JsonDocument doc;
    if (deserializeJson(doc, json_value_str)) {
      return false;
    }
    
    if constexpr (std::is_same_v<value_t, std::string>) {
      if (doc.is<const char *>()) {
        global = std::string(doc.as<const char *>());
        return true;
      }
    } else if constexpr (std::is_same_v<value_t, bool>) {
      if (doc.is<bool>()) {
        global = doc.as<bool>();
        return true;
      }
    } else if constexpr (std::is_floating_point_v<value_t>) {
      if (doc.is<double>()) {
        global = static_cast<T>(doc.as<double>());
        return true;
      }
    } else if constexpr (std::is_integral_v<value_t>) {
      if (doc.is<long long>()) {
        global = static_cast<T>(doc.as<long long>());
        return true;
      }
    }
    return false;
  }

  std::string server_;
  uint16_t port_;
  std::string token_;
  bool auto_telemetry_{true};
  bool attributes_sent_{false};
  std::vector<LightStateEntry> light_states_;
  std::vector<GlobalAttributeEntry> global_attributes_;
  Trigger<std::string, std::string> rpc_trigger_;
  Trigger<std::string, std::string> attribute_trigger_;
  std::string rpc_response_;
#ifdef USE_LOG_LISTENERS
  uint8_t log_level_{ESPHOME_LOG_LEVEL_NONE};
  bool log_listener_registered_{false};
#endif

  bool apply_rpc_to_entity(const std::string& id, JsonVariant value);

  // --- PODMÍNĚNÉ PROMĚNNÉ TŘÍDY ---
#ifdef USE_ARDUINO
  WiFiClientSecure wifiClient;
  PubSubClient mqttClient;
  uint32_t last_reconnect_attempt_{0};
#elif defined(USE_ESP_IDF)
  void initialize_mqtt_();
  void process_pending_events_();
  esp_mqtt_client_handle_t mqtt_client_{nullptr};
  bool is_connected_{false};
  bool mqtt_started_{false};
#endif
};

template<typename... Ts> class SendTelemetryAction : public Action<Ts...> {
 public:
  explicit SendTelemetryAction(ThingsBoardBridge *bridge) : bridge_(bridge) {}
  TEMPLATABLE_VALUE(std::string, key)
  TEMPLATABLE_VALUE(std::string, value)

  void play(Ts... x) override {
    this->bridge_->send_telemetry(this->key_.value(x...), this->value_.value(x...));
  }

 protected:
  ThingsBoardBridge *bridge_;
};

template<typename... Ts> class SendAttributeAction : public Action<Ts...> {
 public:
  explicit SendAttributeAction(ThingsBoardBridge *bridge) : bridge_(bridge) {}
  TEMPLATABLE_VALUE(std::string, key)
  TEMPLATABLE_VALUE(std::string, value)

  void play(Ts... x) override {
    this->bridge_->send_attribute(this->key_.value(x...), this->value_.value(x...));
  }

 protected:
  ThingsBoardBridge *bridge_;
};

}  // namespace thingsboard
}  // namespace esphome
