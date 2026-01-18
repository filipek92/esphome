#pragma once

#include "esphome/core/component.h"
#include "esphome/components/modbus/modbus.h"
#include "esphome/components/socket/socket.h"
#include <vector>
#include <memory>
#include <deque>

namespace esphome {
namespace modbus_proxy {

class ModbusProxy : public modbus::ModbusDevice, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_port(uint16_t port) { port_ = port; }
  
  // ModbusDevice implementation
  void on_modbus_data(const std::vector<uint8_t> &data) override;
  void on_modbus_error(uint8_t function_code, uint8_t exception_code) override;

 protected:
  uint16_t port_{502};
  std::unique_ptr<socket::Socket> server_socket_;
  
  struct Client {
    std::unique_ptr<socket::Socket> socket;
    std::string identifier; // IP:Port for logging
    std::vector<uint8_t> rx_buffer;
    uint32_t last_activity{0};
  };
  std::vector<std::unique_ptr<Client>> clients_;

  struct PendingRequest {
    Client* client; 
    uint16_t transaction_id;
    uint16_t protocol_id;
    uint8_t unit_id;
    uint8_t function_code;
    std::vector<uint8_t> payload; // Payload to send to UART
    uint32_t timestamp;
  };
  
  std::deque<PendingRequest> request_queue_;
  
  bool busy_{false};
  PendingRequest current_request_;

  void process_queue_();
  void check_cleanup_clients_();
  
  void send_tcp_response_(Client *client, uint16_t transaction_id, uint16_t protocol_id, uint8_t unit_id, const std::vector<uint8_t> &pdu);
  void send_tcp_error_(Client *client, uint16_t transaction_id, uint16_t protocol_id, uint8_t unit_id, uint8_t function_code, uint8_t exception_code);
};

} // namespace modbus_proxy
} // namespace esphome
