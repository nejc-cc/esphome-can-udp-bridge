#pragma once

// Emulates a Solarfocus electronic module on a CAN segment owned by a
// can_udp_bridge. The bridge owns the TWAI driver (a singleton) and the
// tunnel batching state, so this component never transmits directly: it
// registers as a CanFrameResponder and the bridge's CAN task asks it to build
// replies, then sends them on the local segment and into the tunnel.
//
// Protocol (reverse-engineered, see solarfocus-protocol-notes.md):
//   poll  0x1E6 + address   [ch, out_lo, out_hi, 0,0,0,0,0]
//   reply 0x220 + address   [ch, d0..d5, address]
// Temperatures are little-endian signed 16-bit in 0.1 degC.

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/can_udp_bridge/can_udp_bridge.h"

#include <atomic>
#include <cmath>

namespace esphome {
namespace sf_module_emulator {

// Poll/reply ID bases. Address 1..3 maps to heating circuits 3/4, 5/6, 7/8.
static constexpr uint32_t SF_POLL_ID_BASE = 0x1E6;
static constexpr uint32_t SF_RESP_ID_BASE = 0x220;
// Sensor value meaning "nothing connected" (+270.0 degC).
static constexpr int16_t SF_SENSOR_ABSENT = 2700;
// ch1 status word. Bits 4/5 = limiting thermostat closed for circuit A/B;
// both must be set or the HMI refuses to run the circuits.
static constexpr uint16_t SF_STATUS_WORD = 0x0530;
// The HMI blanks the whole output word for ~20 ms every 15 s. Ignore changes
// that do not persist at least this long, otherwise driven relays chatter.
static constexpr uint32_t SF_OUTPUT_DEBOUNCE_MS = 100;

// Extra sensor slots, identified by setting one at a time and reading the HMI.
// Populating them unlocks buffer tank, DHW tank and recirculation functions.
static constexpr int SF_NUM_AUX = 7;
enum SfAux {
  SF_AUX_BUFFER_MID = 0,  // ch2 word0 - buffer middle; see note on HMI indexing
  SF_AUX_BUFFER_BOT,      // ch2 word1 - X36 lower buffer sensor (HMI input I6)
  SF_AUX_DHW_TANK,        // ch3 word1 - X39 DHW tank sensor (HMI input I7)
  SF_AUX_CIRCULATION,     // ch4 word0 - recirculation sensor
  SF_AUX_BUFFER_TOP,      // ch4 word1 - X44 upper buffer sensor (HMI input I5)
  SF_AUX_UNUSED_CH4_W2,   // ch4 word2 - no visible effect in the HMI
  SF_AUX_UNUSED_CH5_W0,   // ch5 word0 - no visible effect in the HMI
};

static constexpr int SF_NUM_OUTPUTS = 8;
enum SfOutput {
  SF_OUT_PUMP_A = 0,     // X9  heating circuit pump 3/5/7
  SF_OUT_PUMP_B,         // X10 heating circuit pump 4/6/8
  SF_OUT_MIXER_A_OPEN,   // X11 mixer open  3/5/7
  SF_OUT_MIXER_A_CLOSE,  // X11 mixer close 3/5/7
  SF_OUT_MIXER_B_OPEN,   // X12 mixer open  4/6/8
  SF_OUT_MIXER_B_CLOSE,  // X12 mixer close 4/6/8
  SF_OUT_DHW_PUMP,       // X8  DHW tank charge pump
  SF_OUT_CIRCULATION,    // recirculation pump (once the HMI unlocks it)
};

class SfModuleEmulator : public Component, public can_udp_bridge::CanFrameResponder {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // --- CanFrameResponder ---
  bool wants(uint32_t can_id) const override { return can_id == this->poll_id_; }
  bool handle_frame(uint32_t can_id, const uint8_t *data, uint8_t len,
                    twai_message_t &reply) override;
  void publish_state() override;

  // --- codegen setters ---
  void set_bridge(can_udp_bridge::CanUdpBridge *b) { bridge_ = b; }
  void set_address(uint8_t a) {
    address_ = a;
    poll_id_ = SF_POLL_ID_BASE + a;
    reply_id_ = SF_RESP_ID_BASE + a;
  }
  // Override the address-derived IDs to probe other module families. Observed
  // rule on the heating-circuit family: reply ID = poll ID + 0x3A.
  void set_ids(uint32_t poll_id, uint32_t reply_id) {
    poll_id_ = poll_id;
    reply_id_ = reply_id;
  }
  // type_code -> reply bytes 1-2, module_type -> reply byte 6. Defaults are the
  // heating-circuit module's values; other families need different ones.
  void set_ident(uint16_t type_code, uint8_t module_type) {
    type_code_ = type_code;
    module_type_ = module_type;
  }
  void set_output_sensor(int idx, binary_sensor::BinarySensor *s) {
    if (idx >= 0 && idx < SF_NUM_OUTPUTS)
      out_sensors_[idx] = s;
  }

  // --- runtime API, safe from YAML lambdas ---
  // Flow temperatures in degC. A = circuit 3/5/7 (X38, ch3 word0),
  // B = circuit 4/6/8 (X37, ch2 word2).
  void set_flow_a(float celsius) { flow_a_.store(deci_(celsius)); }
  void set_flow_b(float celsius) { flow_b_.store(deci_(celsius)); }
  float get_flow_a() const { return flow_a_.load() / 10.0f; }
  float get_flow_b() const { return flow_b_.load() / 10.0f; }
  // Aux slots (see SfAux). 270.0 degC reports "not connected".
  void set_aux(int idx, float celsius) {
    if (idx >= 0 && idx < SF_NUM_AUX)
      aux_[idx].store(deci_(celsius));
  }
  uint16_t get_output_word() const { return out_debounced_.load(); }

 protected:
  static int16_t deci_(float c) { return (int16_t) lroundf(c * 10.0f); }

  can_udp_bridge::CanUdpBridge *bridge_{nullptr};
  uint8_t address_{3};
  uint32_t poll_id_{SF_POLL_ID_BASE + 3}, reply_id_{SF_RESP_ID_BASE + 3};
  uint16_t type_code_{0x4BE6};
  uint8_t module_type_{0x00};

  std::atomic<int16_t> flow_a_{SF_SENSOR_ABSENT}, flow_b_{SF_SENSOR_ABSENT};
  std::atomic<int16_t> aux_[SF_NUM_AUX]{{SF_SENSOR_ABSENT}, {SF_SENSOR_ABSENT},
                                        {SF_SENSOR_ABSENT}, {SF_SENSOR_ABSENT},
                                        {SF_SENSOR_ABSENT}, {SF_SENSOR_ABSENT},
                                        {SF_SENSOR_ABSENT}};
  std::atomic<uint16_t> out_debounced_{0};
  std::atomic<uint32_t> replies_{0};

  uint16_t out_pending_{0};          // CAN-task private
  int64_t out_pending_since_us_{0};  // CAN-task private

  binary_sensor::BinarySensor *out_sensors_[SF_NUM_OUTPUTS]{};
  uint32_t last_published_{0xFFFFFFFFUL};  // main-loop private
};

}  // namespace sf_module_emulator
}  // namespace esphome
