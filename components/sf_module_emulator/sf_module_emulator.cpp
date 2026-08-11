#include "sf_module_emulator.h"
#include "esphome/core/log.h"

#include "esp_timer.h"
#include <cstring>

namespace esphome {
namespace sf_module_emulator {

static const char *const TAG = "sf_module_emulator";

void SfModuleEmulator::setup() {
  if (this->bridge_ == nullptr) {
    ESP_LOGE(TAG, "No can_udp_bridge configured");
    this->mark_failed();
    return;
  }
  this->bridge_->add_responder(this);
}

void SfModuleEmulator::dump_config() {
  ESP_LOGCONFIG(TAG, "Solarfocus module emulator:");
  ESP_LOGCONFIG(TAG, "  Address: %u", this->address_);
  ESP_LOGCONFIG(TAG, "  Poll ID: 0x%03X -> reply ID: 0x%03X", (unsigned) this->poll_id_,
                (unsigned) this->reply_id_);
  ESP_LOGCONFIG(TAG, "  Ident: type 0x%04X, module type 0x%02X", (unsigned) this->type_code_,
                (unsigned) this->module_type_);
}

// Runs in the bridge's CAN task, so the turnaround is well under a
// millisecond like the genuine hardware.
bool SfModuleEmulator::handle_frame(uint32_t can_id, const uint8_t *data, uint8_t len,
                                    twai_message_t &reply) {
  if (can_id != this->poll_id_ || len < 3)
    return false;

  const uint8_t ch = data[0];
  const uint16_t out = (uint16_t) data[1] | ((uint16_t) data[2] << 8);

  // Debounce the commanded output word (the HMI blanks it briefly every 15 s).
  const int64_t now_us = esp_timer_get_time();
  if (out != this->out_pending_) {
    this->out_pending_ = out;
    this->out_pending_since_us_ = now_us;
  } else if (this->out_debounced_.load() != out &&
             now_us - this->out_pending_since_us_ >= (int64_t) SF_OUTPUT_DEBOUNCE_MS * 1000) {
    this->out_debounced_.store(out);
  }

  reply.identifier = this->reply_id_;
  reply.data_length_code = 8;
  reply.data[0] = ch;
  reply.data[7] = this->address_;

  // little-endian signed 16-bit, 0.1 degC per count
  auto put = [&reply](int idx, int16_t v) {
    reply.data[idx] = (uint8_t) (v & 0xFF);
    reply.data[idx + 1] = (uint8_t) ((v >> 8) & 0xFF);
  };

  switch (ch) {
    case 0x00:  // identification: type code, SW version 1.31 (BCD),
                // HW rev 1.1 (BCD), module type
      reply.data[1] = (uint8_t) (this->type_code_ & 0xFF);
      reply.data[2] = (uint8_t) ((this->type_code_ >> 8) & 0xFF);
      reply.data[3] = 0x31;
      reply.data[4] = 0x01;
      reply.data[5] = 0x11;
      reply.data[6] = this->module_type_;
      break;
    case 0x01:  // status word (thermostat bits must be set)
      put(1, (int16_t) SF_STATUS_WORD);
      put(3, 0);
      put(5, 0);
      break;
    case 0x02:  // buffer middle, buffer bottom (X36), flow B (X37, circuit 4/6/8)
      put(1, this->aux_[SF_AUX_BUFFER_MID].load());
      put(3, this->aux_[SF_AUX_BUFFER_BOT].load());
      put(5, this->flow_b_.load());
      break;
    case 0x03:  // flow A (X38, circuit 3/5/7), DHW tank (X39)
      put(1, this->flow_a_.load());
      put(3, this->aux_[SF_AUX_DHW_TANK].load());
      put(5, 0);
      break;
    case 0x04:  // circulation, buffer top (X44), and one slot with no HMI effect
      put(1, this->aux_[SF_AUX_CIRCULATION].load());
      put(3, this->aux_[SF_AUX_BUFFER_TOP].load());
      put(5, this->aux_[SF_AUX_UNUSED_CH4_W2].load());
      break;
    case 0x05:  // echoes the commanded output word back to the HMI
      put(1, this->aux_[SF_AUX_UNUSED_CH5_W0].load());
      reply.data[3] = (uint8_t) (out & 0xFF);
      reply.data[4] = (uint8_t) ((out >> 8) & 0xFF);
      reply.data[5] = 0x04;
      reply.data[6] = 0x04;
      break;
    case 0xA0:
      reply.data[1] = 0x03;
      break;
    case 0xA1:
      break;
    case 0xA2:
      reply.data[3] = 0x01;
      break;
    default:
      return false;  // unknown query - a real module stays silent
  }

  this->replies_.fetch_add(1);
  return true;
}

void SfModuleEmulator::publish_state() {
  const uint16_t w = this->out_debounced_.load();
  if (w == this->last_published_)
    return;
  this->last_published_ = w;
  static const uint16_t MASKS[SF_NUM_OUTPUTS] = {
      0x0400,  // SF_OUT_PUMP_A        hi bit2
      0x0800,  // SF_OUT_PUMP_B        hi bit3
      0x0008,  // SF_OUT_MIXER_A_OPEN  lo bit3
      0x0010,  // SF_OUT_MIXER_A_CLOSE lo bit4
      0x0020,  // SF_OUT_MIXER_B_OPEN  lo bit5
      0x0040,  // SF_OUT_MIXER_B_CLOSE lo bit6
      0x0004,  // SF_OUT_DHW_PUMP      lo bit2
      0x0001,  // SF_OUT_CIRCULATION   lo bit0
  };
  for (int i = 0; i < SF_NUM_OUTPUTS; i++) {
    if (this->out_sensors_[i] != nullptr)
      this->out_sensors_[i]->publish_state((w & MASKS[i]) != 0);
  }
}

}  // namespace sf_module_emulator
}  // namespace esphome
