#include "modbus_proxy.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <cerrno>
#include <cinttypes>
#include <cstring>

namespace esphome::modbus_proxy {

static const char *const TAG = "modbus_proxy";

// The hub resolves every accepted request with exactly one terminal callback; this only guards against a
// request that never resolves so the proxy cannot stall forever.
static constexpr uint32_t REQUEST_WATCHDOG_MS = 30000;
static constexpr int LISTEN_BACKLOG = 4;

void ModbusProxy::setup() {
  this->clients_ = std::make_unique<Client[]>(this->max_clients_);
  this->queue_ = std::make_unique<PendingRequest[]>(this->request_queue_limit_);

  this->server_ = socket::socket_ip_loop_monitored(SOCK_STREAM, 0);
  if (this->server_ == nullptr) {
    ESP_LOGE(TAG, "Could not create socket");
    this->mark_failed();
    return;
  }

  int enable = 1;
  this->server_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  if (this->server_->setblocking(false) != 0) {
    ESP_LOGE(TAG, "Could not set socket non-blocking: errno %d", errno);
    this->mark_failed();
    return;
  }

  struct sockaddr_storage server_addr;
  socklen_t addrlen =
      socket::set_sockaddr_any(reinterpret_cast<struct sockaddr *>(&server_addr), sizeof(server_addr), this->port_);
  if (addrlen == 0) {
    ESP_LOGE(TAG, "Could not set sockaddr");
    this->mark_failed();
    return;
  }

  if (this->server_->bind(reinterpret_cast<struct sockaddr *>(&server_addr), addrlen) != 0) {
    ESP_LOGE(TAG, "Could not bind to port %" PRIu16 ": errno %d", this->port_, errno);
    this->mark_failed();
    return;
  }

  if (this->server_->listen(LISTEN_BACKLOG) != 0) {
    ESP_LOGE(TAG, "Could not listen on socket: errno %d", errno);
    this->mark_failed();
    return;
  }

  this->update_clients_connected_();
}

void ModbusProxy::loop() {
  if (this->server_->ready())
    this->accept_new_clients_();

  for (uint8_t i = 0; i < this->max_clients_; i++) {
    if (this->clients_[i].socket != nullptr && this->clients_[i].socket->ready())
      this->read_client_(i);
  }

  this->process_queue_();
}

void ModbusProxy::accept_new_clients_() {
  while (true) {
    struct sockaddr_storage source_addr;
    socklen_t addr_len = sizeof(source_addr);
    auto sock = this->server_->accept_loop_monitored(reinterpret_cast<struct sockaddr *>(&source_addr), &addr_len);
    if (sock == nullptr)
      return;

    Client *slot = nullptr;
    for (uint8_t i = 0; i < this->max_clients_; i++) {
      if (this->clients_[i].socket == nullptr) {
        slot = &this->clients_[i];
        break;
      }
    }

    char peername[socket::SOCKADDR_STR_LEN];
    sock->getpeername_to(peername);
    if (slot == nullptr) {
      ESP_LOGW(TAG, "Rejecting %s: max %" PRIu8 " clients connected", peername, this->max_clients_);
      sock->close();
      continue;
    }

    sock->setblocking(false);
    slot->socket = std::move(sock);
    slot->session = ++this->next_session_;
    slot->rx_len = 0;
    memcpy(slot->peername, peername, sizeof(peername));
    ESP_LOGD(TAG, "Client %s connected", slot->peername);
    this->update_clients_connected_();
  }
}

void ModbusProxy::read_client_(uint8_t index) {
  Client &client = this->clients_[index];
  // Drain the socket until it would block, as required by the Socket::ready() contract
  while (client.socket != nullptr) {
    ssize_t len = client.socket->read(client.rx_buffer.data() + client.rx_len, client.rx_buffer.size() - client.rx_len);
    if (len == 0) {
      this->close_client_(client, LOG_STR("disconnected"));
      return;
    }
    if (len < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        this->close_client_(client, LOG_STR("read error"));
      return;
    }
    client.rx_len += len;
    if (!this->parse_client_frames_(index)) {
      this->close_client_(client, LOG_STR("invalid frame"));
      return;
    }
  }
}

bool ModbusProxy::parse_client_frames_(uint8_t index) {
  Client &client = this->clients_[index];
  const uint8_t *buf = client.rx_buffer.data();

  // MBAP: transaction id(2), protocol id(2), length(2), then `length` bytes of unit id + PDU
  while (client.rx_len >= MBAP_HEADER_SIZE - 1) {
    const uint16_t protocol_id = encode_uint16(buf[2], buf[3]);
    const uint16_t length = encode_uint16(buf[4], buf[5]);
    if (protocol_id != 0) {
      ESP_LOGW(TAG, "Invalid protocol id 0x%04X from %s", protocol_id, client.peername);
      return false;
    }
    // Unit id plus a PDU of at least the function code, bounded by the RTU PDU limit
    if (length < 2 || length > 1 + modbus::MAX_PDU_SIZE) {
      ESP_LOGW(TAG, "Invalid length %" PRIu16 " from %s", length, client.peername);
      return false;
    }
    const uint16_t frame_len = MBAP_HEADER_SIZE - 1 + length;
    if (client.rx_len < frame_len)
      return true;  // wait for the rest; frame_len <= rx_buffer.size(), so it always fits

    const uint16_t transaction_id = encode_uint16(buf[0], buf[1]);
    const uint8_t unit_id = buf[6];
    const uint8_t pdu_len = length - 1;
    const uint8_t *pdu = buf + MBAP_HEADER_SIZE;
    ESP_LOGD(TAG, "Request from %s: tid=%" PRIu16 " unit=%" PRIu8 " fc=0x%02X len=%" PRIu8, client.peername,
             transaction_id, unit_id, pdu[0], pdu_len);

    if (this->queue_count_ >= this->request_queue_limit_) {
      ESP_LOGW(TAG, "Request queue full, rejecting request from %s", client.peername);
      PendingRequest rejected{};
      rejected.session = client.session;
      rejected.transaction_id = transaction_id;
      rejected.client_index = index;
      rejected.unit_id = unit_id;
      this->send_tcp_exception_(rejected, pdu[0], modbus::ExceptionCode::SERVER_DEVICE_BUSY);
    } else {
      PendingRequest &request = this->queue_[(this->queue_head_ + this->queue_count_) % this->request_queue_limit_];
      request.session = client.session;
      request.transaction_id = transaction_id;
      request.client_index = index;
      request.unit_id = unit_id;
      request.pdu_len = pdu_len;
      memcpy(request.pdu.data(), pdu, pdu_len);
      this->queue_count_++;
    }

    // Sending the busy exception may have closed the client on a write failure
    if (client.socket == nullptr)
      return true;

    client.rx_len -= frame_len;
    if (client.rx_len > 0)
      memmove(client.rx_buffer.data(), buf + frame_len, client.rx_len);
  }
  return true;
}

void ModbusProxy::close_client_(Client &client, const LogString *reason) {
  ESP_LOGD(TAG, "Client %s %s", client.peername, LOG_STR_ARG(reason));
  client.socket->close();
  client.socket = nullptr;
  client.rx_len = 0;
  this->update_clients_connected_();
}

ModbusProxy::Client *ModbusProxy::client_for_(const PendingRequest &request) {
  Client &client = this->clients_[request.client_index];
  if (client.socket == nullptr || client.session != request.session)
    return nullptr;
  return &client;
}

void ModbusProxy::process_queue_() {
  if (this->busy_) {
    if (millis() - this->request_start_ > REQUEST_WATCHDOG_MS) {
      ESP_LOGW(TAG, "Request to unit %" PRIu8 " never resolved, giving up", this->current_().unit_id);
      this->clear_tx_queue_for_device();
      this->send_tcp_exception_(this->current_(), this->current_().pdu[0],
                                modbus::ExceptionCode::GATEWAY_TARGET_DEVICE_FAILED_TO_RESPOND);
      this->finish_current_();
    }
    return;
  }

  while (this->queue_count_ > 0) {
    PendingRequest &request = this->current_();
    if (this->client_for_(request) == nullptr) {
      // The requesting client went away while the request was queued
      this->finish_current_();
      continue;
    }

    if (!this->parent_->queue_pdu(request.unit_id, std::span<const uint8_t>(request.pdu.data(), request.pdu_len),
                                  this)) {
      // Refused by the hub (e.g. a non-broadcastable function sent to unit 0); no callback will follow
      this->send_tcp_exception_(request, request.pdu[0], modbus::ExceptionCode::GATEWAY_PATH_UNAVAILABLE);
      this->finish_current_();
      continue;
    }

    this->busy_ = true;
    this->request_start_ = millis();
    return;
  }
}

void ModbusProxy::finish_current_() {
  this->busy_ = false;
  this->queue_head_ = (this->queue_head_ + 1) % this->request_queue_limit_;
  this->queue_count_--;
}

void ModbusProxy::on_response(std::span<const uint8_t> request_pdu, std::span<const uint8_t> response_pdu) {
  if (!this->busy_)
    return;
  // The hub hands over the complete response PDU (function code, byte count and data), which is exactly
  // what the Modbus TCP reply carries after the MBAP header.
  this->send_tcp_frame_(this->current_(), response_pdu);
  this->finish_current_();
}

void ModbusProxy::on_error(std::span<const uint8_t> request_pdu, modbus::ExceptionCode exception_code) {
  if (!this->busy_)
    return;
  this->send_tcp_exception_(this->current_(), request_pdu.empty() ? 0 : request_pdu[0], exception_code);
  this->finish_current_();
}

bool ModbusProxy::on_no_response(std::span<const uint8_t> request_pdu) {
  if (!this->busy_)
    return false;
  ESP_LOGW(TAG, "No response from unit %" PRIu8, this->current_().unit_id);
  this->send_tcp_exception_(this->current_(), request_pdu.empty() ? 0 : request_pdu[0],
                            modbus::ExceptionCode::GATEWAY_TARGET_DEVICE_FAILED_TO_RESPOND);
  this->finish_current_();
  return false;  // the TCP client decides whether to retry
}

void ModbusProxy::on_not_sent(std::span<const uint8_t> request_pdu) {
  if (!this->busy_)
    return;
  this->send_tcp_exception_(this->current_(), request_pdu.empty() ? 0 : request_pdu[0],
                            modbus::ExceptionCode::GATEWAY_PATH_UNAVAILABLE);
  this->finish_current_();
}

void ModbusProxy::on_sent(std::span<const uint8_t> request_pdu) {
  // A broadcast is fire-and-forget: it gets no terminal callback and the TCP client expects no reply
  if (this->busy_ && this->current_().unit_id == modbus::BROADCAST_ADDRESS)
    this->finish_current_();
}

void ModbusProxy::send_tcp_frame_(const PendingRequest &request, std::span<const uint8_t> pdu) {
  Client *client = this->client_for_(request);
  if (client == nullptr) {
    ESP_LOGD(TAG, "Dropping reply for tid=%" PRIu16 ": client gone", request.transaction_id);
    return;
  }
  if (pdu.empty() || pdu.size() > modbus::MAX_PDU_SIZE) {
    ESP_LOGW(TAG, "Invalid reply size %zu", pdu.size());
    return;
  }

  std::array<uint8_t, MAX_TCP_FRAME_SIZE> frame;
  const uint16_t length = 1 + pdu.size();  // unit id + PDU
  frame[0] = request.transaction_id >> 8;
  frame[1] = request.transaction_id & 0xFF;
  frame[2] = 0;  // protocol id
  frame[3] = 0;
  frame[4] = length >> 8;
  frame[5] = length & 0xFF;
  frame[6] = request.unit_id;
  memcpy(frame.data() + MBAP_HEADER_SIZE, pdu.data(), pdu.size());

  const size_t frame_len = MBAP_HEADER_SIZE + pdu.size();
  ESP_LOGV(TAG, "Reply to %s: tid=%" PRIu16 " len=%zu", client->peername, request.transaction_id, frame_len);
  ssize_t written = client->socket->write(frame.data(), frame_len);
  if (written != static_cast<ssize_t>(frame_len)) {
    this->close_client_(*client, LOG_STR("write failed"));
    return;
  }

  this->message_count_++;
  if (this->messages_handled_sensor_ != nullptr)
    this->messages_handled_sensor_->publish_state(this->message_count_);
}

void ModbusProxy::send_tcp_exception_(const PendingRequest &request, uint8_t function_code,
                                      modbus::ExceptionCode exception_code) {
  ESP_LOGW(TAG, "Exception 0x%02X for unit %" PRIu8 " fc=0x%02X tid=%" PRIu16, static_cast<uint8_t>(exception_code),
           request.unit_id, function_code, request.transaction_id);
  this->increment_errors_();

  const std::array<uint8_t, 2> pdu{static_cast<uint8_t>(function_code | modbus::FUNCTION_CODE_EXCEPTION_MASK),
                                   static_cast<uint8_t>(exception_code)};
  this->send_tcp_frame_(request, pdu);
}

void ModbusProxy::increment_errors_() {
  this->error_count_++;
  if (this->errors_sensor_ != nullptr)
    this->errors_sensor_->publish_state(this->error_count_);
}

void ModbusProxy::update_clients_connected_() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < this->max_clients_; i++) {
    if (this->clients_[i].socket != nullptr)
      count++;
  }
  if (count == this->clients_connected_ && this->clients_connected_sensor_ != nullptr &&
      this->clients_connected_sensor_->has_state())
    return;
  this->clients_connected_ = count;
  if (this->clients_connected_sensor_ != nullptr)
    this->clients_connected_sensor_->publish_state(count);
}

void ModbusProxy::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Modbus Proxy:\n"
                "  Port: %" PRIu16 "\n"
                "  Max Clients: %" PRIu8 "\n"
                "  Max Queue Size: %" PRIu8,
                this->port_, this->max_clients_, this->request_queue_limit_);
  LOG_SENSOR("  ", "Clients Connected", this->clients_connected_sensor_);
  LOG_SENSOR("  ", "Messages Handled", this->messages_handled_sensor_);
  LOG_SENSOR("  ", "Errors", this->errors_sensor_);
}

}  // namespace esphome::modbus_proxy
