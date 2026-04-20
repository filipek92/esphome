#include "thingsboard.h"
#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"

#ifdef USE_ESP_IDF
#include "esp_log.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#endif

namespace esphome {
namespace thingsboard {

static const char *const TAG = "thingsboard";

ThingsBoardBridge *global_tb_bridge = nullptr;

// ==============================================================================
// CALLBACKY (Liší se podle frameworku)
// ==============================================================================
#ifdef USE_ARDUINO
void tb_mqtt_callback(char* topic, byte* payload, unsigned int length) {
  if (global_tb_bridge) {
    std::string topic_str(topic);
    std::string payload_str((char*)payload, length);
    if (topic_str == "v1/devices/me/attributes") {
      global_tb_bridge->process_shared_attributes(payload_str);
    } else {
      global_tb_bridge->process_rpc(topic_str, payload_str);
    }
  }
}
#elif defined(USE_ESP_IDF)
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
  ThingsBoardBridge *bridge = (ThingsBoardBridge *)handler_args;

  ThingsBoardBridge::MqttEvent evt;
  evt.event_id = event_id;
  if (event->topic != nullptr && event->topic_len > 0) {
    evt.topic.assign(event->topic, event->topic_len);
  }
  if (event->data != nullptr && event->data_len > 0) {
    evt.data.assign(event->data, event->data_len);
  }

  {
    std::lock_guard<std::mutex> lock(bridge->event_mutex_);
    bridge->pending_events_.push_back(std::move(evt));
  }
}
#endif

ThingsBoardBridge::ThingsBoardBridge(const std::string& server, uint16_t port, const std::string& token)
    : server_(server), port_(port), token_(token) {
#ifdef USE_ARDUINO
  this->mqttClient.setClient(this->wifiClient);
#endif
}

// ==============================================================================
// SETUP A LOOP
// ==============================================================================
void ThingsBoardBridge::setup() {
  ESP_LOGCONFIG(TAG, "Setting up ThingsBoard bridge...");
  ESP_LOGCONFIG(TAG, "  Server: %s:%u", this->server_.c_str(), this->port_);
  global_tb_bridge = this;
  
#ifdef USE_ARDUINO
  this->wifiClient.setInsecure();
  this->mqttClient.setServer(this->server_.c_str(), this->port_);
  this->mqttClient.setCallback(tb_mqtt_callback);
#elif defined(USE_ESP_IDF)
  // ESP-IDF MQTT client is initialized in loop() after network is connected
#endif

  // Registrace společných senzorů (stejné pro oba frameworky)
  if (this->auto_telemetry_) {
#ifdef USE_SENSOR
    for (auto *obj : App.get_sensors()) {
      if (!obj->is_internal()) {
        obj->add_on_state_callback([this, obj](float state) { this->send_telemetry(tb_get_id(obj), state); });
      }
    }
#endif
#ifdef USE_BINARY_SENSOR
    for (auto *obj : App.get_binary_sensors()) {
      if (!obj->is_internal()) {
        obj->add_on_state_callback([this, obj](bool state) { this->send_telemetry(tb_get_id(obj), state); });
      }
    }
#endif
#ifdef USE_NUMBER
    for (auto *obj : App.get_numbers()) {
      if (!obj->is_internal()) {
        obj->add_on_state_callback([this, obj](float state) { this->send_telemetry(tb_get_id(obj), state); });
      }
    }
#endif
#ifdef USE_SWITCH
    for (auto *obj : App.get_switches()) {
      if (!obj->is_internal()) {
        obj->add_on_state_callback([this, obj](bool state) { this->send_telemetry(tb_get_id(obj), state); });
      }
    }
#endif
#ifdef USE_SELECT
    for (auto *obj : App.get_selects()) {
      if (!obj->is_internal()) {
        obj->add_on_state_callback(
            [this, obj](const std::string &state, size_t index) { this->send_telemetry(tb_get_id(obj), state); });
      }
    }
#endif
  }
#ifdef USE_TEXT_SENSOR
  for (auto *obj : App.get_text_sensors()) {
    if (!obj->is_internal()) obj->add_on_state_callback([this, obj](const std::string &state) { this->send_attribute(tb_get_id(obj), state); });
  }
#endif
  if (this->auto_telemetry_) {
#ifdef USE_LIGHT
    for (auto *obj : App.get_lights()) {
      if (!obj->is_internal()) {
        LightStateEntry entry;
        entry.id = tb_get_id(obj);
        entry.is_on = obj->remote_values.is_on();
        this->light_states_.push_back(entry);
      }
    }
#endif
  }

#ifdef USE_LOG_LISTENERS
  if (this->log_level_ > ESPHOME_LOG_LEVEL_NONE && !this->log_listener_registered_ && logger::global_logger != nullptr) {
    logger::global_logger->add_log_callback(
        this, [](void *self, uint8_t level, const char *tag, const char *message, size_t message_len) {
          static_cast<ThingsBoardBridge *>(self)->on_log(level, tag, message, message_len);
        });
    this->log_listener_registered_ = true;
    ESP_LOGD(TAG, "Log forwarding enabled (level %u)", this->log_level_);
  }
#endif
}

void ThingsBoardBridge::dump_config() {
  ESP_LOGCONFIG(TAG, "ThingsBoard Bridge:");
  ESP_LOGCONFIG(TAG, "  Server: %s:%u", this->server_.c_str(), this->port_);
  ESP_LOGCONFIG(TAG, "  Token: %s", this->token_.c_str());
}

void ThingsBoardBridge::loop() {
#ifdef USE_ARDUINO
  if (!this->mqttClient.connected()) {
    if (millis() - this->last_reconnect_attempt_ > 5000) {
      this->last_reconnect_attempt_ = millis();
      if (this->reconnect()) this->last_reconnect_attempt_ = 0;
    }
  } else {
    this->mqttClient.loop();
    if (!this->attributes_sent_) {
      this->send_initial_state();
      this->attributes_sent_ = true;
    }
    if (this->auto_telemetry_) {
#ifdef USE_LIGHT
      for (auto &entry : this->light_states_) {
        // entry.id is a pointer to the object_id string stored by EntityBase
        // find the matching light to check current state
        for (auto *obj : App.get_lights()) {
          if (!obj->is_internal() && tb_get_id(obj) == entry.id) {
            bool is_on = obj->remote_values.is_on();
            if (entry.is_on != is_on) {
              entry.is_on = is_on;
              this->send_telemetry(entry.id, is_on);
            }
            break;
          }
        }
      }
#endif
    }
  }
  if (this->attributes_sent_ && !this->mqttClient.connected()) this->attributes_sent_ = false;

#elif defined(USE_ESP_IDF)
  if (!this->mqtt_started_ && network::is_connected()) {
    this->initialize_mqtt_();
  }
  this->process_pending_events_();
  if (this->is_connected_) {
    if (!this->attributes_sent_) {
      this->send_initial_state();
      this->attributes_sent_ = true;
    }
    if (this->auto_telemetry_) {
#ifdef USE_LIGHT
      for (auto &entry : this->light_states_) {
        for (auto *obj : App.get_lights()) {
          if (!obj->is_internal() && tb_get_id(obj) == entry.id) {
            bool is_on = obj->remote_values.is_on();
            if (entry.is_on != is_on) {
              entry.is_on = is_on;
              this->send_telemetry(entry.id, is_on);
            }
            break;
          }
        }
      }
#endif
    }
  }
#endif
}

#ifdef USE_ARDUINO
bool ThingsBoardBridge::reconnect() {
  ESP_LOGD(TAG, "Attempting MQTT reconnect...");
  if (this->mqttClient.connect(App.get_name().c_str(), this->token_.c_str(), "")) {
    ESP_LOGI(TAG, "MQTT connected");
    this->mqttClient.subscribe("v1/devices/me/rpc/request/+");
    this->mqttClient.subscribe("v1/devices/me/attributes");
    return true;
  }
  return false;
}
#elif defined(USE_ESP_IDF)
void ThingsBoardBridge::initialize_mqtt_() {
  ESP_LOGD(TAG, "Initializing ESP-IDF MQTT client...");
  esp_mqtt_client_config_t mqtt_cfg = {};

#if ESP_IDF_VERSION_MAJOR >= 5
  mqtt_cfg.broker.address.hostname = this->server_.c_str();
  mqtt_cfg.broker.address.port = this->port_;
  mqtt_cfg.credentials.username = this->token_.c_str();
  if (this->port_ == 8883) {
    mqtt_cfg.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
    mqtt_cfg.broker.verification.skip_cert_common_name_check = true;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
#endif
  } else {
    mqtt_cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
  }
#else
  std::string uri = (this->port_ == 8883 ? "mqtts://" : "mqtt://") + this->server_ + ":" + to_string(this->port_);
  mqtt_cfg.uri = uri.c_str();
  mqtt_cfg.username = this->token_.c_str();
  if (this->port_ == 8883) mqtt_cfg.skip_cert_common_name_check = true;
#endif

  this->mqtt_client_ = esp_mqtt_client_init(&mqtt_cfg);
  if (this->mqtt_client_ == nullptr) {
    ESP_LOGE(TAG, "Failed to initialize MQTT client");
    this->mark_failed();
    return;
  }
  esp_mqtt_client_register_event(this->mqtt_client_, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event_handler, this);
  esp_mqtt_client_start(this->mqtt_client_);
  this->mqtt_started_ = true;
  ESP_LOGD(TAG, "ESP-IDF MQTT client started");
}

void ThingsBoardBridge::process_pending_events_() {
  std::vector<MqttEvent> events;
  {
    std::lock_guard<std::mutex> lock(this->event_mutex_);
    std::swap(events, this->pending_events_);
  }
  for (auto &evt : events) {
    switch ((esp_mqtt_event_id_t)evt.event_id) {
      case MQTT_EVENT_CONNECTED:
        this->is_connected_ = true;
        ESP_LOGI(TAG, "MQTT connected");
        esp_mqtt_client_subscribe(this->mqtt_client_, "v1/devices/me/rpc/request/+", 0);
        esp_mqtt_client_subscribe(this->mqtt_client_, "v1/devices/me/attributes", 0);
        break;
      case MQTT_EVENT_DISCONNECTED:
        this->is_connected_ = false;
        this->attributes_sent_ = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
      case MQTT_EVENT_DATA:
        if (evt.topic == "v1/devices/me/attributes") {
          this->process_shared_attributes(evt.data);
        } else {
          this->process_rpc(evt.topic, evt.data);
        }
        break;
      default:
        break;
    }
  }
}
#endif

// ==============================================================================
// SHARED ATTRIBUTES
// ==============================================================================
#ifdef USE_LOG_LISTENERS
static uint8_t parse_log_level(const char *level_str) {
  if (strcasecmp(level_str, "ERROR") == 0) return ESPHOME_LOG_LEVEL_ERROR;
  if (strcasecmp(level_str, "WARN") == 0) return ESPHOME_LOG_LEVEL_WARN;
  if (strcasecmp(level_str, "INFO") == 0) return ESPHOME_LOG_LEVEL_INFO;
  if (strcasecmp(level_str, "CONFIG") == 0) return ESPHOME_LOG_LEVEL_CONFIG;
  if (strcasecmp(level_str, "DEBUG") == 0) return ESPHOME_LOG_LEVEL_DEBUG;
  if (strcasecmp(level_str, "VERBOSE") == 0) return ESPHOME_LOG_LEVEL_VERBOSE;
  if (strcasecmp(level_str, "VERY_VERBOSE") == 0) return ESPHOME_LOG_LEVEL_VERY_VERBOSE;
  if (strcasecmp(level_str, "NONE") == 0) return ESPHOME_LOG_LEVEL_NONE;
  return ESPHOME_LOG_LEVEL_NONE;
}
#endif

void ThingsBoardBridge::process_shared_attributes(const std::string &payload) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error)
    return;

  // Zavolat uživatelský trigger pro každý atribut
  JsonObject root = doc.as<JsonObject>();
  for (JsonPair kv : root) {
    std::string value_str;
    serializeJson(kv.value(), value_str);
    this->attribute_trigger_.trigger(std::string(kv.key().c_str()), value_str);
  }

#ifdef USE_LOG_LISTENERS
  if (doc["log_level"].is<const char *>()) {
    uint8_t new_level = parse_log_level(doc["log_level"].as<const char *>());
    ESP_LOGI(TAG, "Log level changed to %u via shared attribute", new_level);
    this->log_level_ = new_level;

    // Zaregistrovat listener pokud ještě nebyl
    if (new_level > ESPHOME_LOG_LEVEL_NONE && !this->log_listener_registered_ && logger::global_logger != nullptr) {
      logger::global_logger->add_log_callback(
          this, [](void *self, uint8_t level, const char *tag, const char *message, size_t message_len) {
            static_cast<ThingsBoardBridge *>(self)->on_log(level, tag, message, message_len);
          });
      this->log_listener_registered_ = true;
    }
  }
#endif
}

// ==============================================================================
// SPOLEČNÁ LOGIKA (Univerzální aplikátor a Parser RPC nezávislý na frameworku)
// ==============================================================================
bool ThingsBoardBridge::apply_rpc_to_entity(const std::string& id, JsonVariant value) {
#ifdef USE_SWITCH
  for (auto *sw : App.get_switches()) {
    if (!sw->is_internal() && tb_get_id(sw) == id && value.is<bool>()) {
      if (value.as<bool>()) sw->turn_on(); else sw->turn_off();
      return true;
    }
  }
#endif
#ifdef USE_NUMBER
  for (auto *num : App.get_numbers()) {
    if (!num->is_internal() && tb_get_id(num) == id && (value.is<float>() || value.is<int>())) {
      auto call = num->make_call();
      call.set_value(value.as<float>());
      call.perform();
      return true;
    }
  }
#endif
#ifdef USE_SELECT
  for (auto *sel : App.get_selects()) {
    if (!sel->is_internal() && tb_get_id(sel) == id && value.is<const char*>()) {
      auto call = sel->make_call();
      call.set_option(value.as<std::string>());
      call.perform();
      return true;
    }
  }
#endif
#ifdef USE_BUTTON
  for (auto *btn : App.get_buttons()) {
    if (!btn->is_internal() && tb_get_id(btn) == id) {
      btn->press();
      return true;
    }
  }
#endif
#ifdef USE_LIGHT
  for (auto *light : App.get_lights()) {
    if (!light->is_internal() && tb_get_id(light) == id) {
      auto call = light->make_call();
      if (value.is<bool>()) {
        call.set_state(value.as<bool>());
      } else if (value.is<JsonObject>()) {
        JsonObject l_params = value.as<JsonObject>();
        if (l_params["state"].is<bool>()) call.set_state(l_params["state"].as<bool>());
        if (l_params["brightness"].is<float>()) call.set_brightness(l_params["brightness"].as<float>() / 255.0);
      }
      call.perform();
      return true;
    }
  }
#endif
  return false;
}

void ThingsBoardBridge::process_rpc(std::string topic, std::string payload) {
  size_t request_id_pos = topic.find_last_of('/');
  std::string request_id = (request_id_pos != std::string::npos) ? topic.substr(request_id_pos + 1) : "";

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (error || !doc["method"].is<const char *>()) return;

  const char* method = doc["method"];
  bool action_performed = false;

  // Zavolat uživatelský trigger
  std::string params_str;
  serializeJson(doc["params"], params_str);
  this->rpc_response_.clear();
  this->rpc_trigger_.trigger(std::string(method), params_str);

  if (doc["params"].is<JsonObject>()) {
    JsonObject params = doc["params"].as<JsonObject>();
    for (JsonPair kv : params) {
      if (this->apply_rpc_to_entity(kv.key().c_str(), kv.value())) {
        action_performed = true;
      }
    }
  } else if (!doc["params"].isNull()) {
    if (this->apply_rpc_to_entity(method, doc["params"])) {
      action_performed = true;
    }
  }

  std::string response_topic = "v1/devices/me/rpc/response/" + request_id;
  std::string response_payload;
  if (!this->rpc_response_.empty()) {
    response_payload = this->rpc_response_;
  } else {
    response_payload = action_performed ? "{\"success\":true}" : "{\"error\":\"Unknown method\"}";
  }
  
#ifdef USE_ARDUINO
  this->mqttClient.publish(response_topic.c_str(), response_payload.c_str());
#elif defined(USE_ESP_IDF)
  esp_mqtt_client_publish(this->mqtt_client_, response_topic.c_str(), response_payload.c_str(), 0, 0, 0);
#endif
}

// ==============================================================================
// ODESÍLÁNÍ TELEMETRIE (Wrapper pro odeslání dle frameworku)
// ==============================================================================
void ThingsBoardBridge::send_device_attributes() {
  char buf[256];
  snprintf(buf, sizeof(buf), "{\"device_name\": \"%s\", \"esphome_version\": \"%s\", \"build_date\": \"%s %s\"}", 
           App.get_name().c_str(), ESPHOME_VERSION, __DATE__, __TIME__);
#ifdef USE_ARDUINO
  this->mqttClient.publish("v1/devices/me/attributes", buf);
#elif defined(USE_ESP_IDF)
  esp_mqtt_client_publish(this->mqtt_client_, "v1/devices/me/attributes", buf, 0, 0, 0);
#endif

  for (const auto &entry : this->global_attributes_) {
    this->send_attribute(entry.key, entry.value_getter());
  }
}

void ThingsBoardBridge::send_initial_state() {
  ESP_LOGI(TAG, "Sending initial state...");
  this->send_device_attributes();

  if (!this->auto_telemetry_) {
    return;
  }

#ifdef USE_SENSOR
  for (auto *obj : App.get_sensors()) {
    if (!obj->is_internal() && obj->has_state()) this->send_telemetry(tb_get_id(obj), obj->state);
  }
#endif
#ifdef USE_BINARY_SENSOR
  for (auto *obj : App.get_binary_sensors()) {
    if (!obj->is_internal() && obj->has_state()) this->send_telemetry(tb_get_id(obj), obj->state);
  }
#endif
#ifdef USE_NUMBER
  for (auto *obj : App.get_numbers()) {
    if (!obj->is_internal() && obj->has_state()) this->send_telemetry(tb_get_id(obj), obj->state);
  }
#endif
#ifdef USE_SWITCH
  for (auto *obj : App.get_switches()) {
    if (!obj->is_internal()) this->send_telemetry(tb_get_id(obj), obj->state);
  }
#endif
#ifdef USE_SELECT
  for (auto *obj : App.get_selects()) {
    if (!obj->is_internal() && obj->has_state()) this->send_telemetry(tb_get_id(obj), obj->state);
  }
#endif
#ifdef USE_TEXT_SENSOR
  for (auto *obj : App.get_text_sensors()) {
    if (!obj->is_internal() && obj->has_state()) this->send_attribute(tb_get_id(obj), obj->state);
  }
#endif
#ifdef USE_LIGHT
  for (auto &entry : this->light_states_) {
    this->send_telemetry(entry.id, entry.is_on);
  }
#endif
}

void ThingsBoardBridge::send_telemetry(std::string id, float value) {
  bool can_send = false;
#ifdef USE_ARDUINO
  can_send = this->mqttClient.connected();
#elif defined(USE_ESP_IDF)
  can_send = this->is_connected_;
#endif

  if (can_send && !std::isnan(value)) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"%s\": %.2f}", id.c_str(), value);
#ifdef USE_ARDUINO
    this->mqttClient.publish("v1/devices/me/telemetry", buf);
#elif defined(USE_ESP_IDF)
    esp_mqtt_client_publish(this->mqtt_client_, "v1/devices/me/telemetry", buf, 0, 0, 0);
#endif
  }
}

void ThingsBoardBridge::send_telemetry(std::string id, bool value) {
  bool can_send = false;
#ifdef USE_ARDUINO
  can_send = this->mqttClient.connected();
#elif defined(USE_ESP_IDF)
  can_send = this->is_connected_;
#endif

  if (can_send) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"%s\": %s}", id.c_str(), value ? "true" : "false");
#ifdef USE_ARDUINO
    this->mqttClient.publish("v1/devices/me/telemetry", buf);
#elif defined(USE_ESP_IDF)
    esp_mqtt_client_publish(this->mqtt_client_, "v1/devices/me/telemetry", buf, 0, 0, 0);
#endif
  }
}

void ThingsBoardBridge::send_telemetry(std::string id, std::string value) {
  bool can_send = false;
#ifdef USE_ARDUINO
  can_send = this->mqttClient.connected();
#elif defined(USE_ESP_IDF)
  can_send = this->is_connected_;
#endif

  if (can_send) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"%s\": \"%s\"}", id.c_str(), value.c_str());
#ifdef USE_ARDUINO
    this->mqttClient.publish("v1/devices/me/telemetry", buf);
#elif defined(USE_ESP_IDF)
    esp_mqtt_client_publish(this->mqtt_client_, "v1/devices/me/telemetry", buf, 0, 0, 0);
#endif
  }
}

void ThingsBoardBridge::send_attribute(std::string id, std::string value) {
  bool can_send = false;
#ifdef USE_ARDUINO
  can_send = this->mqttClient.connected();
#elif defined(USE_ESP_IDF)
  can_send = this->is_connected_;
#endif

  if (can_send) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"%s\": \"%s\"}", id.c_str(), value.c_str());
#ifdef USE_ARDUINO
    this->mqttClient.publish("v1/devices/me/attributes", buf);
#elif defined(USE_ESP_IDF)
    esp_mqtt_client_publish(this->mqtt_client_, "v1/devices/me/attributes", buf, 0, 0, 0);
#endif
  }
}

void ThingsBoardBridge::send_attribute(std::string id, float value) {
  bool can_send = false;
#ifdef USE_ARDUINO
  can_send = this->mqttClient.connected();
#elif defined(USE_ESP_IDF)
  can_send = this->is_connected_;
#endif

  if (can_send && !std::isnan(value)) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"%s\": %.2f}", id.c_str(), value);
#ifdef USE_ARDUINO
    this->mqttClient.publish("v1/devices/me/attributes", buf);
#elif defined(USE_ESP_IDF)
    esp_mqtt_client_publish(this->mqtt_client_, "v1/devices/me/attributes", buf, 0, 0, 0);
#endif
  }
}

void ThingsBoardBridge::send_attribute(std::string id, bool value) {
  bool can_send = false;
#ifdef USE_ARDUINO
  can_send = this->mqttClient.connected();
#elif defined(USE_ESP_IDF)
  can_send = this->is_connected_;
#endif

  if (can_send) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"%s\": %s}", id.c_str(), value ? "true" : "false");
#ifdef USE_ARDUINO
    this->mqttClient.publish("v1/devices/me/attributes", buf);
#elif defined(USE_ESP_IDF)
    esp_mqtt_client_publish(this->mqtt_client_, "v1/devices/me/attributes", buf, 0, 0, 0);
#endif
  }
}

// ==============================================================================
// LOG FORWARDING
// ==============================================================================
#ifdef USE_LOG_LISTENERS
void ThingsBoardBridge::on_log(uint8_t level, const char *tag, const char *message, size_t message_len) {
  if (level > this->log_level_)
    return;

  // Ignorovat vlastní logy, aby se zabránilo rekurzi
  if (strcmp(tag, TAG) == 0)
    return;

  bool can_send = false;
#ifdef USE_ARDUINO
  can_send = this->mqttClient.connected();
#elif defined(USE_ESP_IDF)
  can_send = this->is_connected_;
#endif

  if (!can_send)
    return;

  // Escapovat zprávu pro JSON
  char buf[512];
  size_t pos = 0;
  pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"log\":\"");
  for (size_t i = 0; i < message_len && pos < sizeof(buf) - 4; i++) {
    char c = message[i];
    if (c == '"' || c == '\\') {
      buf[pos++] = '\\';
      buf[pos++] = c;
    } else if (c == '\n') {
      buf[pos++] = '\\';
      buf[pos++] = 'n';
    } else if (c >= 0x20) {
      buf[pos++] = c;
    }
  }
  pos += snprintf(buf + pos, sizeof(buf) - pos, "\"}");

#ifdef USE_ARDUINO
  this->mqttClient.publish("v1/devices/me/telemetry", buf);
#elif defined(USE_ESP_IDF)
  esp_mqtt_client_publish(this->mqtt_client_, "v1/devices/me/telemetry", buf, 0, 0, 0);
#endif
}
#endif

}  // namespace thingsboard
}  // namespace esphome
