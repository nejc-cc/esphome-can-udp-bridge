#pragma once

// CAN <-> UDP tunnel using cannelloni-compatible framing (v2, op DATA).
// Wire format: header {version u8=2, op_code u8=0, seq_no u8, count u16 BE},
// then per frame {can_id u32 BE (EFF=0x80000000, RTR=0x40000000), len u8, data[len]}.
// Byte-compatible with `cannelloni -C c -l 20000 -R <bridge_ip>` + vcan on Linux.

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <atomic>
#include <string>
#include <vector>

#include <cmath>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/twai.h"
#include "lwip/sockets.h"

namespace esphome {
namespace can_udp_bridge {

enum class BridgeMode : uint8_t { LISTEN_ONLY = 0, BRIDGE = 1 };

enum class BusState : uint8_t { STOPPED = 0, RUNNING = 1, RECOVERING = 2, BUS_OFF = 3 };

static constexpr uint8_t CNL_VERSION = 2;
static constexpr uint8_t CNL_OP_DATA = 0;
static constexpr uint32_t CNL_EFF_FLAG = 0x80000000UL;
static constexpr uint32_t CNL_RTR_FLAG = 0x40000000UL;
static constexpr uint32_t CNL_SFF_MASK = 0x000007FFUL;
static constexpr uint32_t CNL_EFF_MASK = 0x1FFFFFFFUL;

// Maximum tunnel endpoints in the mesh (this node's peers, excluding itself).
static constexpr int MAX_PEERS = 4;

// Maximum device emulators (or other frame responders) attached to one bridge.
static constexpr int MAX_RESPONDERS = 4;

// A frame arriving from the tunnel, handed to the CAN task for answering.
struct QueuedFrame {
  uint32_t id;
  uint8_t len;
  uint8_t data[8];
};

// Lets other components answer CAN frames on this bridge's segment (e.g. a
// device emulator) without owning the TWAI driver, which is a singleton and
// whose batching state is only safe to touch from the bridge's CAN task.
class CanFrameResponder {
 public:
  virtual ~CanFrameResponder() = default;
  // Cheap filter, called from the UDP task to decide whether a tunnelled frame
  // needs handing over to the CAN task at all.
  virtual bool wants(uint32_t can_id) const = 0;
  // Called from the CAN task. Return true and populate `reply` to answer.
  virtual bool handle_frame(uint32_t can_id, const uint8_t *data, uint8_t len,
                            twai_message_t &reply) = 0;
  // Called from the main loop, for publishing entity state.
  virtual void publish_state() = 0;
};

class CanUdpBridge : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::LATE; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  // codegen setters
  void set_pins(int tx, int rx) {
    tx_pin_ = tx;
    rx_pin_ = rx;
  }
  void set_bit_rate(uint32_t bps) { bit_rate_ = bps; }
  void set_mode(BridgeMode m) { mode_ = m; }
  void add_peer(const std::string &ip) { peer_ip_strs_.push_back(ip); }
  void set_port(uint16_t port) { port_ = port; }
  void set_buffer_frames(uint8_t n) { buffer_frames_ = n; }
  void set_buffer_timeout(uint32_t ms) { buffer_timeout_ms_ = ms; }
  void set_peer_timeout(uint32_t ms) { peer_timeout_ms_ = ms; }
  void set_keepalive_interval(uint32_t ms) { keepalive_ms_ = ms; }

  void set_rx_rate_sensor(sensor::Sensor *s) { rx_rate_sensor_ = s; }
  void set_tx_rate_sensor(sensor::Sensor *s) { tx_rate_sensor_ = s; }
  void set_bus_errors_sensor(sensor::Sensor *s) { bus_errors_sensor_ = s; }
  void set_rx_missed_sensor(sensor::Sensor *s) { rx_missed_sensor_ = s; }
  void set_tx_failed_sensor(sensor::Sensor *s) { tx_failed_sensor_ = s; }
  void set_udp_lost_sensor(sensor::Sensor *s) { udp_lost_sensor_ = s; }
  void set_peer_alive_sensor(binary_sensor::BinarySensor *s) { peer_alive_sensor_ = s; }
  void set_bus_state_sensor(text_sensor::TextSensor *s) { bus_state_sensor_ = s; }
  void set_last_frame_sensor(text_sensor::TextSensor *s) { last_frame_sensor_ = s; }

  // runtime control — callable from YAML lambdas (main loop context); the CAN
  // task owns the TWAI driver lifecycle, these only request a reconfig
  void request_bit_rate(uint32_t bps);
  void request_mode(BridgeMode m);
  void reset_counters();  // zero all diagnostic counters (main loop context)

  // Attach a frame responder (e.g. sf_module_emulator). Call before setup().
  void add_responder(CanFrameResponder *r) {
    if (this->responder_count_ < MAX_RESPONDERS)
      this->responders_[this->responder_count_++] = r;
  }

  uint32_t get_bit_rate() const { return pending_bitrate_.load(); }
  BridgeMode get_mode() const { return static_cast<BridgeMode>(pending_mode_.load()); }

 protected:
  // Full mesh: every local CAN frame is unicast to each peer, and packets are
  // accepted from any of them. Sequence numbers are tracked per peer.
  // (Declared before the methods that take it — C++ needs the type first.)
  struct PeerState {
    struct sockaddr_in addr {};
    std::atomic<int64_t> last_rx_us{0};
    bool seq_valid{false};  // udp_task-private
    uint8_t seq_no{0};      // udp_task-private
  };

  static void can_task_trampoline(void *pv);
  static void udp_task_trampoline(void *pv);
  void can_task_();
  void udp_task_();
  bool twai_install_and_start_();
  void twai_stop_and_uninstall_();
  void batch_append_(const twai_message_t &msg);
  void batch_flush_();
  void handle_udp_payload_(const uint8_t *buf, size_t len, PeerState &peer);
  int find_peer_(const struct sockaddr_in &src);
  void store_last_frame_(const twai_message_t &msg);
  // can_task context only: offer a frame to every responder and send any reply
  void dispatch_responders_(uint32_t id, const uint8_t *data, uint8_t len);
  bool any_responder_wants_(uint32_t id) const;  // udp_task context

  // config
  int tx_pin_{32}, rx_pin_{33};
  uint32_t bit_rate_{100000};
  BridgeMode mode_{BridgeMode::BRIDGE};
  std::vector<std::string> peer_ip_strs_;
  uint16_t port_{20000};
  uint8_t buffer_frames_{8};
  uint32_t buffer_timeout_ms_{2}, peer_timeout_ms_{5000}, keepalive_ms_{1000};

  // sensors (all optional)
  sensor::Sensor *rx_rate_sensor_{nullptr}, *tx_rate_sensor_{nullptr}, *bus_errors_sensor_{nullptr},
      *rx_missed_sensor_{nullptr}, *tx_failed_sensor_{nullptr}, *udp_lost_sensor_{nullptr};
  binary_sensor::BinarySensor *peer_alive_sensor_{nullptr};
  text_sensor::TextSensor *bus_state_sensor_{nullptr}, *last_frame_sensor_{nullptr};

  // runtime state
  int sock_{-1};
  PeerState peers_[MAX_PEERS];
  uint8_t peer_count_{0};
  TaskHandle_t can_task_handle_{nullptr}, udp_task_handle_{nullptr};
  std::atomic<bool> reconfig_flag_{false};
  std::atomic<uint32_t> pending_bitrate_{100000};
  std::atomic<uint8_t> pending_mode_{static_cast<uint8_t>(BridgeMode::BRIDGE)};
  std::atomic<bool> driver_ok_{false};

  // cannelloni TX batching (can_task-private)
  uint8_t udp_buf_[512];
  size_t udp_len_{0};
  uint16_t udp_frame_count_{0};
  uint8_t tx_seq_no_{0};
  int64_t first_frame_us_{0};
  int64_t last_udp_tx_us_{0};

  // stats (written by tasks, read by loop())
  std::atomic<uint32_t> can_rx_total_{0}, can_tx_total_{0}, tx_failed_{0}, rx_missed_total_{0},
      bus_error_total_{0}, udp_lost_{0}, udp_send_err_{0};
  std::atomic<int64_t> last_peer_rx_us_{0};
  std::atomic<uint8_t> bus_state_{static_cast<uint8_t>(BusState::STOPPED)};
  SemaphoreHandle_t last_frame_mutex_{nullptr};
  char last_frame_str_[80]{};
  std::atomic<bool> last_frame_dirty_{false};

  // --- attached frame responders ---
  CanFrameResponder *responders_[MAX_RESPONDERS]{};
  uint8_t responder_count_{0};
  QueueHandle_t responder_queue_{nullptr};  // tunnelled frames awaiting a reply

  // loop() bookkeeping
  uint32_t prev_rx_total_{0}, prev_tx_total_{0};
  uint32_t last_publish_ms_{0};
};

}  // namespace can_udp_bridge
}  // namespace esphome
