#include "can_udp_bridge.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

#include "esp_timer.h"
#include "lwip/inet.h"

#include <cstring>
#include <cstdio>

namespace esphome {
namespace can_udp_bridge {

static const char *const TAG = "can_udp_bridge";

static constexpr size_t CNL_HEADER_LEN = 5;

// How long the TWAI driver may sit outside RUNNING before we force a full
// reinstall instead of waiting for recovery that may never complete.
static constexpr uint32_t DRIVER_STUCK_TIMEOUT_MS = 10000;

// ---------------------------------------------------------------------------
// ESPHome lifecycle (main task)
// ---------------------------------------------------------------------------

void CanUdpBridge::setup() {
  this->pending_bitrate_.store(this->bit_rate_);
  this->pending_mode_.store(static_cast<uint8_t>(this->mode_));

  this->last_frame_mutex_ = xSemaphoreCreateMutex();

  for (const auto &ip : this->peer_ip_strs_) {
    if (this->peer_count_ >= MAX_PEERS) {
      ESP_LOGE(TAG, "More than %d peers configured, ignoring '%s'", MAX_PEERS, ip.c_str());
      break;
    }
    PeerState &p = this->peers_[this->peer_count_];
    memset(&p.addr, 0, sizeof(p.addr));
    p.addr.sin_family = AF_INET;
    p.addr.sin_port = htons(this->port_);
    if (inet_aton(ip.c_str(), &p.addr.sin_addr) == 0) {
      ESP_LOGE(TAG, "Invalid peer IP '%s'", ip.c_str());
      this->mark_failed();
      return;
    }
    this->peer_count_++;
  }
  if (this->peer_count_ == 0) {
    ESP_LOGE(TAG, "No valid peers configured");
    this->mark_failed();
    return;
  }

  this->sock_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (this->sock_ < 0) {
    ESP_LOGE(TAG, "socket() failed: errno %d", errno);
    this->mark_failed();
    return;
  }
  struct sockaddr_in local;
  memset(&local, 0, sizeof(local));
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(this->port_);
  if (bind(this->sock_, (struct sockaddr *) &local, sizeof(local)) < 0) {
    ESP_LOGE(TAG, "bind() failed: errno %d", errno);
    this->mark_failed();
    return;
  }

  if (this->responder_count_ > 0)
    this->responder_queue_ = xQueueCreate(8, sizeof(QueuedFrame));

  xTaskCreatePinnedToCore(CanUdpBridge::can_task_trampoline, "can_bridge_can", 6144, this, 10,
                          &this->can_task_handle_, 1);
  xTaskCreatePinnedToCore(CanUdpBridge::udp_task_trampoline, "can_bridge_udp", 6144, this, 10,
                          &this->udp_task_handle_, 1);
}

void CanUdpBridge::dump_config() {
  ESP_LOGCONFIG(TAG, "CAN UDP Bridge:");
  ESP_LOGCONFIG(TAG, "  TX pin: GPIO%d, RX pin: GPIO%d", this->tx_pin_, this->rx_pin_);
  ESP_LOGCONFIG(TAG, "  Bit rate: %" PRIu32 " bps", this->pending_bitrate_.load());
  ESP_LOGCONFIG(TAG, "  Mode: %s",
                this->get_mode() == BridgeMode::LISTEN_ONLY ? "listen_only" : "bridge");
  ESP_LOGCONFIG(TAG, "  Port: %u, peers: %u", this->port_, this->peer_count_);
  for (uint8_t i = 0; i < this->peer_count_; i++)
    ESP_LOGCONFIG(TAG, "    peer[%u]: %s", i, inet_ntoa(this->peers_[i].addr.sin_addr));
  ESP_LOGCONFIG(TAG, "  Batch: %u frames / %" PRIu32 " ms", this->buffer_frames_,
                this->buffer_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Frame responders: %u", this->responder_count_);
}

void CanUdpBridge::request_bit_rate(uint32_t bps) {
  ESP_LOGI(TAG, "Bit rate change requested: %" PRIu32 " bps", bps);
  this->pending_bitrate_.store(bps);
  this->reconfig_flag_.store(true);
}

void CanUdpBridge::request_mode(BridgeMode m) {
  ESP_LOGI(TAG, "Mode change requested: %s", m == BridgeMode::LISTEN_ONLY ? "listen_only" : "bridge");
  this->pending_mode_.store(static_cast<uint8_t>(m));
  this->reconfig_flag_.store(true);
}

// ---------------------------------------------------------------------------
// Frame responders (device emulators)
// ---------------------------------------------------------------------------

bool CanUdpBridge::any_responder_wants_(uint32_t id) const {
  for (uint8_t i = 0; i < this->responder_count_; i++) {
    if (this->responders_[i]->wants(id))
      return true;
  }
  return false;
}

// Offer a frame to every responder. A reply goes out on the local segment AND
// into the tunnel: the controller never receives its own transmissions, so a
// peer on another segment would otherwise never hear it.
void CanUdpBridge::dispatch_responders_(uint32_t id, const uint8_t *data, uint8_t len) {
  for (uint8_t i = 0; i < this->responder_count_; i++) {
    twai_message_t reply;
    memset(&reply, 0, sizeof(reply));
    if (!this->responders_[i]->handle_frame(id, data, len, reply))
      continue;
    twai_transmit(&reply, pdMS_TO_TICKS(10));
    this->batch_append_(reply);
    this->batch_flush_();
  }
}

void CanUdpBridge::reset_counters() {
  this->can_rx_total_.store(0);
  this->can_tx_total_.store(0);
  this->tx_failed_.store(0);
  this->rx_missed_total_.store(0);
  this->bus_error_total_.store(0);
  this->udp_lost_.store(0);
  this->udp_send_err_.store(0);
  this->prev_rx_total_ = 0;
  this->prev_tx_total_ = 0;
  ESP_LOGI(TAG, "Counters reset");
}

void CanUdpBridge::loop() {
  // Responder entity state is published as soon as it changes, not on the
  // 1 Hz statistics cadence below.
  for (uint8_t i = 0; i < this->responder_count_; i++)
    this->responders_[i]->publish_state();

  const uint32_t now = millis();
  if (now - this->last_publish_ms_ < 1000)
    return;
  const float dt = (now - this->last_publish_ms_) / 1000.0f;
  this->last_publish_ms_ = now;

  const uint32_t rx = this->can_rx_total_.load();
  const uint32_t tx = this->can_tx_total_.load();
  if (this->rx_rate_sensor_ != nullptr)
    this->rx_rate_sensor_->publish_state((rx - this->prev_rx_total_) / dt);
  if (this->tx_rate_sensor_ != nullptr)
    this->tx_rate_sensor_->publish_state((tx - this->prev_tx_total_) / dt);
  this->prev_rx_total_ = rx;
  this->prev_tx_total_ = tx;

  if (this->bus_errors_sensor_ != nullptr)
    this->bus_errors_sensor_->publish_state(this->bus_error_total_.load());
  if (this->rx_missed_sensor_ != nullptr)
    this->rx_missed_sensor_->publish_state(this->rx_missed_total_.load());
  if (this->tx_failed_sensor_ != nullptr)
    this->tx_failed_sensor_->publish_state(this->tx_failed_.load());
  if (this->udp_lost_sensor_ != nullptr)
    this->udp_lost_sensor_->publish_state(this->udp_lost_.load());

  if (this->peer_alive_sensor_ != nullptr) {
    // "alive" only when EVERY peer is reachable — a partially-connected mesh
    // means some CAN segment is cut off, which must not read as healthy.
    const int64_t now_us = esp_timer_get_time();
    bool all_alive = this->peer_count_ > 0;
    for (uint8_t i = 0; i < this->peer_count_; i++) {
      const int64_t last = this->peers_[i].last_rx_us.load();
      if (last == 0 || (now_us - last) >= (int64_t) this->peer_timeout_ms_ * 1000) {
        all_alive = false;
        break;
      }
    }
    this->peer_alive_sensor_->publish_state(all_alive);
  }

  if (this->bus_state_sensor_ != nullptr) {
    const char *state_str = "STOPPED";
    switch (static_cast<BusState>(this->bus_state_.load())) {
      case BusState::RUNNING:
        state_str = "RUNNING";
        break;
      case BusState::RECOVERING:
        state_str = "RECOVERING";
        break;
      case BusState::BUS_OFF:
        state_str = "BUS_OFF";
        break;
      default:
        break;
    }
    if (!this->bus_state_sensor_->has_state() || this->bus_state_sensor_->state != state_str)
      this->bus_state_sensor_->publish_state(state_str);
  }

  if (this->last_frame_sensor_ != nullptr && this->last_frame_dirty_.exchange(false)) {
    char buf[sizeof(this->last_frame_str_)];
    if (xSemaphoreTake(this->last_frame_mutex_, pdMS_TO_TICKS(5)) == pdTRUE) {
      strncpy(buf, this->last_frame_str_, sizeof(buf));
      buf[sizeof(buf) - 1] = '\0';
      xSemaphoreGive(this->last_frame_mutex_);
      this->last_frame_sensor_->publish_state(buf);
    }
  }
}

// ---------------------------------------------------------------------------
// TWAI driver lifecycle (owned by can_task)
// ---------------------------------------------------------------------------

bool CanUdpBridge::twai_install_and_start_() {
  const uint32_t bps = this->pending_bitrate_.load();
  const auto mode = static_cast<BridgeMode>(this->pending_mode_.load());

  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
      (gpio_num_t) this->tx_pin_, (gpio_num_t) this->rx_pin_,
      mode == BridgeMode::LISTEN_ONLY ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL);
  g.rx_queue_len = 32;
  g.tx_queue_len = 16;
  g.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED | TWAI_ALERT_ERR_PASS |
                     TWAI_ALERT_BUS_ERROR | TWAI_ALERT_RX_QUEUE_FULL | TWAI_ALERT_RX_FIFO_OVERRUN;

  twai_timing_config_t t;
  switch (bps) {
    case 50000:
      t = TWAI_TIMING_CONFIG_50KBITS();
      break;
    case 125000:
      t = TWAI_TIMING_CONFIG_125KBITS();
      break;
    case 250000:
      t = TWAI_TIMING_CONFIG_250KBITS();
      break;
    case 100000:
    default:
      t = TWAI_TIMING_CONFIG_100KBITS();
      break;
  }
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t err = twai_driver_install(&g, &t, &f);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "twai_driver_install failed: %s", esp_err_to_name(err));
    return false;
  }
  err = twai_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "twai_start failed: %s", esp_err_to_name(err));
    twai_driver_uninstall();
    return false;
  }
  this->mode_ = mode;
  this->bit_rate_ = bps;
  this->bus_state_.store(static_cast<uint8_t>(BusState::RUNNING));
  this->driver_ok_.store(true);
  ESP_LOGI(TAG, "TWAI started: %" PRIu32 " bps, %s mode", bps,
           mode == BridgeMode::LISTEN_ONLY ? "listen-only" : "normal");
  return true;
}

void CanUdpBridge::twai_stop_and_uninstall_() {
  this->driver_ok_.store(false);
  // uninstall is only legal from STOPPED/BUS_OFF — from RECOVERING it fails,
  // so retry briefly rather than leaving a half-torn-down driver behind.
  for (int attempt = 0; attempt < 20; attempt++) {
    twai_stop();  // no-op unless RUNNING
    if (twai_driver_uninstall() == ESP_OK)
      break;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  this->bus_state_.store(static_cast<uint8_t>(BusState::STOPPED));
}

// ---------------------------------------------------------------------------
// CAN -> UDP (can_task)
// ---------------------------------------------------------------------------

void CanUdpBridge::can_task_trampoline(void *pv) { static_cast<CanUdpBridge *>(pv)->can_task_(); }

void CanUdpBridge::store_last_frame_(const twai_message_t &msg) {
  char buf[sizeof(this->last_frame_str_)];
  int pos = snprintf(buf, sizeof(buf), "%s0x%0*X [%d]%s", msg.extd ? "EXT " : "",
                     msg.extd ? 8 : 3, (unsigned) msg.identifier, (int) msg.data_length_code,
                     msg.rtr ? " RTR" : "");
  if (!msg.rtr) {
    for (int i = 0; i < msg.data_length_code && pos < (int) sizeof(buf) - 4; i++)
      pos += snprintf(buf + pos, sizeof(buf) - pos, " %02X", msg.data[i]);
  }
  if (xSemaphoreTake(this->last_frame_mutex_, 0) == pdTRUE) {
    strncpy(this->last_frame_str_, buf, sizeof(this->last_frame_str_));
    this->last_frame_str_[sizeof(this->last_frame_str_) - 1] = '\0';
    xSemaphoreGive(this->last_frame_mutex_);
    this->last_frame_dirty_.store(true);
  }
}

void CanUdpBridge::batch_append_(const twai_message_t &msg) {
  if (this->udp_frame_count_ == 0) {
    // reserve header space
    this->udp_len_ = CNL_HEADER_LEN;
    this->first_frame_us_ = esp_timer_get_time();
  }
  uint32_t id = msg.identifier & (msg.extd ? CNL_EFF_MASK : CNL_SFF_MASK);
  if (msg.extd)
    id |= CNL_EFF_FLAG;
  if (msg.rtr)
    id |= CNL_RTR_FLAG;
  const uint32_t id_be = htonl(id);
  memcpy(this->udp_buf_ + this->udp_len_, &id_be, 4);
  this->udp_len_ += 4;
  this->udp_buf_[this->udp_len_++] = msg.data_length_code;
  if (!msg.rtr) {
    memcpy(this->udp_buf_ + this->udp_len_, msg.data, msg.data_length_code);
    this->udp_len_ += msg.data_length_code;
  }
  this->udp_frame_count_++;
}

void CanUdpBridge::batch_flush_() {
  this->udp_buf_[0] = CNL_VERSION;
  this->udp_buf_[1] = CNL_OP_DATA;
  this->udp_buf_[2] = this->tx_seq_no_++;
  const uint16_t count_be = htons(this->udp_frame_count_);
  memcpy(this->udp_buf_ + 3, &count_be, 2);

  // full mesh: the same datagram goes to every peer
  for (uint8_t i = 0; i < this->peer_count_; i++) {
    const ssize_t sent = sendto(this->sock_, this->udp_buf_, this->udp_len_, 0,
                                (struct sockaddr *) &this->peers_[i].addr,
                                sizeof(this->peers_[i].addr));
    if (sent < 0)
      this->udp_send_err_.fetch_add(1);
  }
  this->last_udp_tx_us_ = esp_timer_get_time();
  this->udp_frame_count_ = 0;
  this->udp_len_ = 0;
}

void CanUdpBridge::can_task_() {
  int64_t last_status_us = 0;
  int64_t unhealthy_since_us = 0;
  uint32_t prev_bus_err = 0, prev_rx_missed = 0, prev_rx_overrun = 0;

  if (!this->twai_install_and_start_()) {
    // retry until it succeeds (e.g. transient GPIO/driver issue)
    while (!this->twai_install_and_start_())
      vTaskDelay(pdMS_TO_TICKS(1000));
  }
  this->last_udp_tx_us_ = esp_timer_get_time();

  while (true) {
    // --- frames handed over from the UDP task (master on a remote segment) ---
    if (this->responder_queue_ != nullptr) {
      QueuedFrame qf;
      while (xQueueReceive(this->responder_queue_, &qf, 0) == pdTRUE)
        this->dispatch_responders_(qf.id, qf.data, qf.len);
    }

    // --- runtime reconfiguration (this task owns the driver) ---
    if (this->reconfig_flag_.exchange(false)) {
      this->twai_stop_and_uninstall_();
      prev_bus_err = prev_rx_missed = prev_rx_overrun = 0;
      while (!this->twai_install_and_start_())
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // --- alert handling ---
    uint32_t alerts = 0;
    if (twai_read_alerts(&alerts, 0) == ESP_OK) {
      if (alerts & TWAI_ALERT_BUS_OFF) {
        ESP_LOGW(TAG, "Bus-off detected, initiating recovery");
        this->driver_ok_.store(false);
        this->bus_state_.store(static_cast<uint8_t>(BusState::BUS_OFF));
        if (twai_initiate_recovery() == ESP_OK)
          this->bus_state_.store(static_cast<uint8_t>(BusState::RECOVERING));
      }
      if (alerts & TWAI_ALERT_BUS_RECOVERED) {
        // Never assume the restart worked: an unchecked twai_start() failure
        // used to latch the driver in a dead state while reporting RUNNING,
        // which needed a manual reboot to clear.
        const esp_err_t serr = twai_start();
        if (serr == ESP_OK) {
          ESP_LOGI(TAG, "Bus recovered, driver restarted");
          this->driver_ok_.store(true);
          this->bus_state_.store(static_cast<uint8_t>(BusState::RUNNING));
        } else {
          ESP_LOGE(TAG, "Bus recovered but twai_start failed: %s — will reinstall",
                   esp_err_to_name(serr));
        }
      }
    }

    // --- error counters from driver status, sampled 1/s ---
    const int64_t now_us = esp_timer_get_time();
    if (now_us - last_status_us >= 1000000) {
      last_status_us = now_us;
      twai_status_info_t status;
      if (twai_get_status_info(&status) == ESP_OK) {
        if (status.bus_error_count >= prev_bus_err)
          this->bus_error_total_.fetch_add(status.bus_error_count - prev_bus_err);
        prev_bus_err = status.bus_error_count;
        if (status.rx_missed_count >= prev_rx_missed)
          this->rx_missed_total_.fetch_add(status.rx_missed_count - prev_rx_missed);
        prev_rx_missed = status.rx_missed_count;
        if (status.rx_overrun_count >= prev_rx_overrun)
          this->rx_missed_total_.fetch_add(status.rx_overrun_count - prev_rx_overrun);
        prev_rx_overrun = status.rx_overrun_count;

        // Trust the driver, not our own flags: a sustained non-RUNNING state
        // (e.g. repeated bus-off from a duplicate-address collision storm) is
        // cleared by a full reinstall rather than waiting for a human.
        if (status.state == TWAI_STATE_RUNNING) {
          unhealthy_since_us = 0;
          this->driver_ok_.store(true);
          this->bus_state_.store(static_cast<uint8_t>(BusState::RUNNING));
        } else {
          this->driver_ok_.store(false);
          this->bus_state_.store(static_cast<uint8_t>(
              status.state == TWAI_STATE_BUS_OFF     ? BusState::BUS_OFF
              : status.state == TWAI_STATE_RECOVERING ? BusState::RECOVERING
                                                      : BusState::STOPPED));
          if (unhealthy_since_us == 0) {
            unhealthy_since_us = now_us;
          } else if (now_us - unhealthy_since_us >= (int64_t) DRIVER_STUCK_TIMEOUT_MS * 1000) {
            ESP_LOGE(TAG, "TWAI stuck out of RUNNING for %" PRIu32 " ms — reinstalling driver",
                     DRIVER_STUCK_TIMEOUT_MS);
            this->twai_stop_and_uninstall_();
            while (!this->twai_install_and_start_())
              vTaskDelay(pdMS_TO_TICKS(1000));
            prev_bus_err = prev_rx_missed = prev_rx_overrun = 0;
            unhealthy_since_us = 0;
          }
        }
      }
    }

    // --- receive; returns the instant a frame arrives (per-frame forwarding),
    // the timeout only caps the idle wait and doubles as the keepalive tick.
    // Min 1 tick: pdMS_TO_TICKS(1) truncates to 0 at low tick rates, which
    // would busy-spin this task and starve the idle task (watchdog reset).
    constexpr TickType_t rx_wait = pdMS_TO_TICKS(1) > 0 ? pdMS_TO_TICKS(1) : 1;
    twai_message_t msg;
    const esp_err_t rerr = twai_receive(&msg, rx_wait);
    if (rerr == ESP_OK) {
      this->can_rx_total_.fetch_add(1);
      this->store_last_frame_(msg);
      this->batch_append_(msg);
      // A frame heard directly on the local segment (master on this side).
      if (this->responder_count_ > 0 && !msg.extd && !msg.rtr)
        this->dispatch_responders_(msg.identifier, msg.data, msg.data_length_code);
    } else if (rerr != ESP_ERR_TIMEOUT) {
      // twai_receive() only blocks while the driver is RUNNING; in bus-off,
      // recovering or stopped it returns immediately, which turned this loop
      // into a 100%-CPU spin that starved the idle task.
      this->driver_ok_.store(false);
      vTaskDelay(pdMS_TO_TICKS(10));
    }

    const int64_t now2_us = esp_timer_get_time();
    if (this->udp_frame_count_ > 0 &&
        (this->udp_frame_count_ >= this->buffer_frames_ ||
         now2_us - this->first_frame_us_ >= (int64_t) this->buffer_timeout_ms_ * 1000)) {
      this->batch_flush_();
    }
    if (this->udp_frame_count_ == 0 &&
        now2_us - this->last_udp_tx_us_ >= (int64_t) this->keepalive_ms_ * 1000) {
      // empty DATA packet (count=0) as keepalive
      this->udp_len_ = CNL_HEADER_LEN;
      this->udp_frame_count_ = 0;
      this->batch_flush_();
    }
  }
}

// ---------------------------------------------------------------------------
// UDP -> CAN (udp_task)
// ---------------------------------------------------------------------------

void CanUdpBridge::udp_task_trampoline(void *pv) { static_cast<CanUdpBridge *>(pv)->udp_task_(); }

int CanUdpBridge::find_peer_(const struct sockaddr_in &src) {
  for (uint8_t i = 0; i < this->peer_count_; i++) {
    if (this->peers_[i].addr.sin_addr.s_addr == src.sin_addr.s_addr)
      return i;
  }
  return -1;
}

void CanUdpBridge::handle_udp_payload_(const uint8_t *buf, size_t len, PeerState &peer) {
  if (len < CNL_HEADER_LEN || buf[0] != CNL_VERSION || buf[1] != CNL_OP_DATA)
    return;

  const uint8_t seq = buf[2];
  if (peer.seq_valid) {
    const uint8_t gap = (uint8_t) (seq - (uint8_t) (peer.seq_no + 1));
    if (gap > 0 && gap < 128)  // treat large "gaps" as peer restart, not loss
      this->udp_lost_.fetch_add(gap);
  }
  peer.seq_no = seq;
  peer.seq_valid = true;

  uint16_t count;
  memcpy(&count, buf + 3, 2);
  count = ntohs(count);

  size_t pos = CNL_HEADER_LEN;
  for (uint16_t i = 0; i < count; i++) {
    if (pos + 5 > len)
      return;  // malformed
    uint32_t id;
    memcpy(&id, buf + pos, 4);
    id = ntohl(id);
    pos += 4;
    const uint8_t dlc = buf[pos++];
    if (dlc > TWAI_FRAME_MAX_DLC)
      return;  // malformed / CAN-FD not supported

    twai_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.extd = (id & CNL_EFF_FLAG) ? 1 : 0;
    msg.rtr = (id & CNL_RTR_FLAG) ? 1 : 0;
    msg.identifier = id & (msg.extd ? CNL_EFF_MASK : CNL_SFF_MASK);
    msg.data_length_code = dlc;
    if (!msg.rtr) {
      if (pos + dlc > len)
        return;  // malformed
      memcpy(msg.data, buf + pos, dlc);
      pos += dlc;
    }

    // A frame that arrived over the tunnel (master on a remote segment). The
    // reply must be built by the CAN task — batching state is not thread-safe —
    // so hand it over rather than answering here.
    if (this->responder_queue_ != nullptr && !msg.extd && !msg.rtr &&
        this->any_responder_wants_(msg.identifier)) {
      QueuedFrame qf;
      qf.id = msg.identifier;
      qf.len = msg.data_length_code;
      memcpy(qf.data, msg.data, 8);
      xQueueSend(this->responder_queue_, &qf, 0);
    }

    if (static_cast<BridgeMode>(this->pending_mode_.load()) != BridgeMode::BRIDGE ||
        !this->driver_ok_.load())
      continue;  // listen-only or driver reconfiguring: consume, don't transmit
    if (twai_transmit(&msg, pdMS_TO_TICKS(10)) == ESP_OK) {
      this->can_tx_total_.fetch_add(1);
    } else {
      this->tx_failed_.fetch_add(1);
    }
  }
}

void CanUdpBridge::udp_task_() {
  uint8_t buf[1500];
  struct sockaddr_in src;
  socklen_t src_len;

  while (true) {
    src_len = sizeof(src);
    const ssize_t n =
        recvfrom(this->sock_, buf, sizeof(buf), 0, (struct sockaddr *) &src, &src_len);
    if (n <= 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    const int pi = this->find_peer_(src);
    if (pi < 0)
      continue;  // not one of our peers — drop silently
    const int64_t now = esp_timer_get_time();
    this->peers_[pi].last_rx_us.store(now);
    this->last_peer_rx_us_.store(now);
    this->handle_udp_payload_(buf, (size_t) n, this->peers_[pi]);
  }
}

}  // namespace can_udp_bridge
}  // namespace esphome
