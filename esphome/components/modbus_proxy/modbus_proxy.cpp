#include "modbus_proxy.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include <cerrno>

namespace esphome {
namespace modbus_proxy {

static const char *const TAG = "modbus_proxy";

void ModbusProxy::setup() {
  struct sockaddr_storage server_addr;
  socklen_t addrlen = socket::set_sockaddr_any((struct sockaddr *)&server_addr, sizeof(server_addr), this->port_);

  this->server_socket_ = socket::socket_ip(SOCK_STREAM, 0);
  if (this->server_socket_ == nullptr) {
    ESP_LOGE(TAG, "Could not create socket");
    this->mark_failed();
    return;
  }
  
  int enable = 1;
  this->server_socket_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

  if (this->server_socket_->bind((struct sockaddr *)&server_addr, addrlen) != 0) {
    ESP_LOGE(TAG, "Could not bind socket to port %d. errno: %d", this->port_, errno);
    this->mark_failed();
    return;
  }

  if (this->server_socket_->listen(5) != 0) {
     ESP_LOGE(TAG, "Could not listen on socket. errno: %d", errno);
     this->mark_failed();
     return;
  }
  
  this->server_socket_->setblocking(false);
  ESP_LOGI(TAG, "Modbus Proxy listening on port %d", this->port_);
}

void ModbusProxy::loop() {
  if (this->server_socket_ == nullptr) return;

  // Accept new clients
  struct sockaddr_storage source_addr;
  socklen_t addr_len = sizeof(source_addr);
  std::unique_ptr<socket::Socket> client_sock = this->server_socket_->accept((struct sockaddr *)&source_addr, &addr_len);
  
  if (client_sock) {
     client_sock->setblocking(false);
     auto client = std::make_unique<Client>();
     client->socket = std::move(client_sock);
     
     char addr_str[socket::SOCKADDR_STR_LEN];
     client->socket->getpeername_to(addr_str);
     client->identifier = addr_str;
     client->last_activity = millis();
     
     ESP_LOGD(TAG, "New connection from %s", client->identifier.c_str());
     this->clients_.push_back(std::move(client));
  }
  
  // Process clients
  for (auto &client : this->clients_) {
     if (client->socket == nullptr) continue;

     uint8_t buf[128];
     ssize_t len = client->socket->read(buf, sizeof(buf));
     if (len > 0) {
        client->last_activity = millis();
        client->rx_buffer.insert(client->rx_buffer.end(), buf, buf + len);
        
        while (client->rx_buffer.size() >= 6) {
             // Check header: TransID(2), ProtoID(2), Len(2)
             uint16_t proto_id = (client->rx_buffer[2] << 8) | client->rx_buffer[3];
             if (proto_id != 0) {
                 ESP_LOGW(TAG, "Invalid Protocol Key %04X from %s", proto_id, client->identifier.c_str());
                 client->rx_buffer.clear(); 
                 break;
             }
             
             uint16_t msg_len = (client->rx_buffer[4] << 8) | client->rx_buffer[5];
             if (msg_len < 2) { 
                 ESP_LOGW(TAG, "Invalid Length %d from %s", msg_len, client->identifier.c_str());
                 client->rx_buffer.erase(client->rx_buffer.begin(), client->rx_buffer.begin() + 6);
                 continue;
             }
             
             if (client->rx_buffer.size() >= 6 + msg_len) {
                 // Full frame received
                 uint16_t trans_id = (client->rx_buffer[0] << 8) | client->rx_buffer[1];
                 uint8_t unit_id = client->rx_buffer[6];
                 uint8_t func_code = client->rx_buffer[7];
                 
                 std::vector<uint8_t> payload;
                 // Payload for send_raw: [UnitID][PDU...]
                 payload.insert(payload.end(), client->rx_buffer.begin() + 6, client->rx_buffer.begin() + 6 + msg_len);
                 
                 PendingRequest req;
                 req.client = client.get();
                 req.transaction_id = trans_id;
                 req.protocol_id = proto_id;
                 req.unit_id = unit_id;
                 req.function_code = func_code;
                 req.payload = payload;
                 req.timestamp = millis();
                 
                 if (payload.size() >= 6) {
                      uint16_t start_addr = (payload[2] << 8) | payload[3];
                      uint16_t count = (payload[4] << 8) | payload[5];
                      if (func_code >= 0x01 && func_code <= 0x04) {
                           ESP_LOGD(TAG, "Modbus Request: UnitID=%d Func=%d StartAddr=0x%04X Count=%d", unit_id, func_code, start_addr, count);
                      } else if (func_code == 0x05 || func_code == 0x06) {
                           ESP_LOGD(TAG, "Modbus Request: UnitID=%d Func=%d Addr=0x%04X Value=0x%04X", unit_id, func_code, start_addr, count);
                      } else if (func_code == 0x0F || func_code == 0x10) {
                           ESP_LOGD(TAG, "Modbus Request: UnitID=%d Func=%d StartAddr=0x%04X Count=%d ByteCount=%d", unit_id, func_code, start_addr, count, payload.size() > 6 ? payload[6] : 0);
                      } else {
                           ESP_LOGD(TAG, "Modbus Request: UnitID=%d Func=%d Len=%d", unit_id, func_code, msg_len);
                      }
                 } else {
                      ESP_LOGD(TAG, "Modbus Request: UnitID=%d Func=%d Len=%d", unit_id, func_code, msg_len);
                 }
                 ESP_LOGV(TAG, "Modbus TCP Request: UnitID=%d Func=%d TransID=%d Len=%d", unit_id, func_code, trans_id, msg_len);

                 this->request_queue_.push_back(req);
                 
                 // Consume
                 client->rx_buffer.erase(client->rx_buffer.begin(), client->rx_buffer.begin() + 6 + msg_len);
             } else {
                 break; // Wait for more data
             }
        }
     } else if (len == 0 || (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
         ESP_LOGD(TAG, "Client %s disconnected", client->identifier.c_str());
         client->socket->close();
         client->socket = nullptr;
     }
  }
  
  this->process_queue_();
  this->check_cleanup_clients_();
}

void ModbusProxy::process_queue_() {
    if (this->busy_) {
        // Check timeout
        if (millis() - this->current_request_.timestamp > 2000) { 
             ESP_LOGW(TAG, "Transaction timed out");
             this->busy_ = false;
        }
        return;
    }
    
    // Check if Modbus parent is busy
    if (this->parent_->waiting_for_response != nullptr) return; 
    
    if (this->request_queue_.empty()) return;
    
    this->current_request_ = this->request_queue_.front();
    this->request_queue_.pop_front();
    
    // Verify client still exists
    bool client_valid = false;
    for(auto &c : this->clients_) {
        if (c->socket != nullptr && c.get() == this->current_request_.client) {
            client_valid = true;
            break;
        }
    }
    
    if (!client_valid) {
        return;
    }
    
    this->busy_ = true;
    this->current_request_.timestamp = millis();
    this->set_address(this->current_request_.unit_id); 
    
    ESP_LOGV(TAG, "Sending Modbus RTU Request to UnitID=%d", this->current_request_.unit_id);
    this->parent_->send_raw(this->current_request_.payload, this);
}

void ModbusProxy::on_modbus_data(const std::vector<uint8_t> &data) {
    if (!this->busy_) return;
    
    bool client_valid = false;
    for(auto &c : this->clients_) {
        if (c->socket != nullptr && c.get() == this->current_request_.client) {
            client_valid = true;
            break;
        }
    }
    if (!client_valid) {
       this->busy_ = false;
       return;
    }

    std::vector<uint8_t> pdu;
    pdu.push_back(this->current_request_.function_code);
    
    // For read operations, modbus component strips the byte count, but Modbus TCP needs it.
    // We strictly use raw numbers here because ModbusFunctionCode enum might not be visible or scoped
    if (this->current_request_.function_code == 0x01 || // READ_COILS
        this->current_request_.function_code == 0x02 || // READ_DISCRETE_INPUTS
        this->current_request_.function_code == 0x03 || // READ_HOLDING_REGISTERS
        this->current_request_.function_code == 0x04) { // READ_INPUT_REGISTERS
        pdu.push_back(data.size());
    }

    pdu.insert(pdu.end(), data.begin(), data.end());
    
    ESP_LOGV(TAG, "Received Modbus RTU Response");

    this->send_tcp_response_(this->current_request_.client, 
                             this->current_request_.transaction_id,
                             this->current_request_.protocol_id,
                             this->current_request_.unit_id,
                             pdu);
                             
    this->busy_ = false;
}

void ModbusProxy::on_modbus_error(uint8_t function_code, uint8_t exception_code) {
    if (!this->busy_) return;
    
     bool client_valid = false;
    for(auto &c : this->clients_) {
        if (c->socket != nullptr && c.get() == this->current_request_.client) {
            client_valid = true;
            break;
        }
    }
    if (!client_valid) {
       this->busy_ = false;
       return;
    }

    this->send_tcp_error_(this->current_request_.client,
                          this->current_request_.transaction_id,
                          this->current_request_.protocol_id,
                          this->current_request_.unit_id,
                          function_code,
                          exception_code);
    this->busy_ = false;
}

void ModbusProxy::send_tcp_response_(Client *client, uint16_t transaction_id, uint16_t protocol_id, uint8_t unit_id, const std::vector<uint8_t> &pdu) {
    std::vector<uint8_t> frame;
    frame.push_back(transaction_id >> 8);
    frame.push_back(transaction_id & 0xFF);
    frame.push_back(protocol_id >> 8);
    frame.push_back(protocol_id & 0xFF);
    
    uint16_t len = 1 + pdu.size(); // UnitID + PDU
    frame.push_back(len >> 8);
    frame.push_back(len & 0xFF);
    
    frame.push_back(unit_id);
    frame.insert(frame.end(), pdu.begin(), pdu.end());
    
    ESP_LOGV(TAG, "Sending Modbus TCP Response TransID=%d Len=%zu", transaction_id, frame.size());

    client->socket->write(frame.data(), frame.size());
}

void ModbusProxy::send_tcp_error_(Client *client, uint16_t transaction_id, uint16_t protocol_id, uint8_t unit_id, uint8_t function_code, uint8_t exception_code) {
    std::vector<uint8_t> frame;
    frame.push_back(transaction_id >> 8);
    frame.push_back(transaction_id & 0xFF);
    frame.push_back(protocol_id >> 8);
    frame.push_back(protocol_id & 0xFF);
    
    uint16_t len = 3; // UnitID + FC + Exception
    frame.push_back(len >> 8);
    frame.push_back(len & 0xFF);
    
    frame.push_back(unit_id);
    frame.push_back(function_code | 0x80); // Error flag
    frame.push_back(exception_code);
    
    ESP_LOGW(TAG, "Sending Modbus TCP Error TransID=%d Exception=%d", transaction_id, exception_code);

    client->socket->write(frame.data(), frame.size());
}

void ModbusProxy::check_cleanup_clients_() {
    this->clients_.erase(std::remove_if(this->clients_.begin(), this->clients_.end(),
        [](const std::unique_ptr<Client> &c) {
             return c->socket == nullptr; 
        }), this->clients_.end());
}

void ModbusProxy::dump_config() {
     ESP_LOGCONFIG(TAG, "Modbus Proxy:");
     ESP_LOGCONFIG(TAG, "  Port: %d", this->port_);
}

}
}
