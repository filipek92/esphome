#pragma once

#include "esphome/core/component.h"
#include "esphome/components/modbus/modbus.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/socket/socket.h"

#include <array>
#include <memory>
#include <span>

namespace esphome::modbus_proxy {

// MBAP header: transaction id(2) + protocol id(2) + length(2) + unit id(1)
static constexpr uint16_t MBAP_HEADER_SIZE = 7;
static constexpr uint16_t MAX_TCP_FRAME_SIZE = MBAP_HEADER_SIZE + modbus::MAX_PDU_SIZE;

/// Modbus TCP -> RTU gateway. Accepts Modbus TCP clients and forwards each request, one at a time,
/// to the RTU bus through the modbus client hub, then relays the reply back to the originating client.
class ModbusProxy : public Component, public modbus::ModbusClientDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_port(uint16_t port) { this->port_ = port; }
  void set_max_clients(uint8_t max_clients) { this->max_clients_ = max_clients; }
  void set_request_queue_limit(uint8_t limit) { this->request_queue_limit_ = limit; }
  void set_clients_connected_sensor(sensor::Sensor *sensor) { this->clients_connected_sensor_ = sensor; }
  void set_messages_handled_sensor(sensor::Sensor *sensor) { this->messages_handled_sensor_ = sensor; }
  void set_errors_sensor(sensor::Sensor *sensor) { this->errors_sensor_ = sensor; }

  // ModbusClientDevice callbacks
  void on_response(std::span<const uint8_t> request_pdu, std::span<const uint8_t> response_pdu) override;
  void on_error(std::span<const uint8_t> request_pdu, modbus::ExceptionCode exception_code) override;
  bool on_no_response(std::span<const uint8_t> request_pdu) override;
  void on_not_sent(std::span<const uint8_t> request_pdu) override;
  void on_sent(std::span<const uint8_t> request_pdu) override;

 protected:
  struct Client {
    std::unique_ptr<socket::Socket> socket;
    // Incremented on every accept into this slot so queued requests from a previous connection are dropped
    uint32_t session{0};
    uint16_t rx_len{0};
    char peername[socket::SOCKADDR_STR_LEN]{};
    std::array<uint8_t, MAX_TCP_FRAME_SIZE> rx_buffer;
  };

  struct PendingRequest {
    uint32_t session;
    uint16_t transaction_id;
    uint8_t client_index;
    uint8_t unit_id;
    uint8_t pdu_len;
    std::array<uint8_t, modbus::MAX_PDU_SIZE> pdu;
  };

  void accept_new_clients_();
  void read_client_(uint8_t index);
  // Returns false if the client sent an invalid frame and must be disconnected
  bool parse_client_frames_(uint8_t index);
  void close_client_(Client &client, const LogString *reason);
  void process_queue_();
  void finish_current_();
  void update_clients_connected_();

  Client *client_for_(const PendingRequest &request);
  PendingRequest &current_() { return this->queue_[this->queue_head_]; }

  void send_tcp_frame_(const PendingRequest &request, std::span<const uint8_t> pdu);
  void send_tcp_exception_(const PendingRequest &request, uint8_t function_code, modbus::ExceptionCode exception_code);
  void increment_errors_();

  std::unique_ptr<socket::ListenSocket> server_;
  std::unique_ptr<Client[]> clients_;
  // Ring buffer of requests; the head entry is the one in flight while busy_ is set
  std::unique_ptr<PendingRequest[]> queue_;

  sensor::Sensor *clients_connected_sensor_{nullptr};
  sensor::Sensor *messages_handled_sensor_{nullptr};
  sensor::Sensor *errors_sensor_{nullptr};

  uint32_t next_session_{0};
  uint32_t message_count_{0};
  uint32_t error_count_{0};
  uint32_t request_start_{0};
  uint16_t port_{502};
  uint8_t max_clients_{2};
  uint8_t request_queue_limit_{10};
  uint8_t queue_head_{0};
  uint8_t queue_count_{0};
  uint8_t clients_connected_{0};
  bool busy_{false};
};

}  // namespace esphome::modbus_proxy
