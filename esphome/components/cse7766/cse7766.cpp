#include "cse7766.h"
#include "esphome/core/log.h"
#include <cinttypes>
#include <iomanip>
#include <sstream>

namespace esphome {
namespace cse7766 {

static const char *const TAG = "cse7766";

void CSE7766Component::loop() {
  const uint32_t now = millis();
  if (now - this->last_transmission_ >= 500) {
    // last transmission too long ago. Reset RX index.
    this->raw_data_index_ = 0;
  }

  if (this->available() == 0) {
    return;
  }

  this->last_transmission_ = now;
  while (this->available() != 0) {
    this->read_byte(&this->raw_data_[this->raw_data_index_]);
    if (!this->check_byte_()) {
      this->raw_data_index_ = 0;
      this->status_set_warning();
      continue;
    }

    if (this->raw_data_index_ == 23) {
      this->parse_data_();
      this->status_clear_warning();
    }

    this->raw_data_index_ = (this->raw_data_index_ + 1) % 24;
  }
}
float CSE7766Component::get_setup_priority() const { return setup_priority::DATA; }

bool CSE7766Component::check_byte_() {
  uint8_t index = this->raw_data_index_;
  uint8_t byte = this->raw_data_[index];
  if (index == 0) {
    return !((byte != 0x55) && ((byte & 0xF0) != 0xF0) && (byte != 0xAA));
  }

  if (index == 1) {
    if (byte != 0x5A) {
      ESP_LOGV(TAG, "Invalid Header 2 Start: 0x%02X!", byte);
      return false;
    }
    return true;
  }

  if (index == 23) {
    uint8_t checksum = 0;
    for (uint8_t i = 2; i < 23; i++) {
      checksum += this->raw_data_[i];
    }

    if (checksum != this->raw_data_[23]) {
      ESP_LOGW(TAG, "Invalid checksum from CSE7766: 0x%02X != 0x%02X", checksum, this->raw_data_[23]);
      return false;
    }
    return true;
  }

  return true;
}
void CSE7766Component::parse_data_() {
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERY_VERBOSE
  {
    std::stringstream ss;
    ss << "Raw data:" << std::hex << std::uppercase << std::setfill('0');
    for (uint8_t i = 0; i < 23; i++) {
      ss << ' ' << std::setw(2) << static_cast<unsigned>(this->raw_data_[i]);
    }
    ESP_LOGVV(TAG, "%s", ss.str().c_str());
  }
#endif

  // Parse header

  uint8_t header1 = this->raw_data_[0];
  uint8_t header2 = this->raw_data_[1];

  bool power_cycle_exceeds_range = false;

  if (header1 == 0xAA) {
    ESP_LOGE(TAG, "CSE7766 not calibrated!");
    return;
  }
  if ((header1 & 0xF0) == 0xF0) {
    if (header1 & 0xD) {
      ESP_LOGE(TAG, "CSE7766 reports abnormal external circuit or chip damage: (0x%02X)", header1);
      if (header1 & (1 << 3)) {
        ESP_LOGE(TAG, "  Voltage cycle exceeds range.");
      }
      if (header1 & (1 << 2)) {
        ESP_LOGE(TAG, "  Current cycle exceeds range.");
      }
      if (header1 & (1 << 0)) {
        ESP_LOGE(TAG, "  Coefficient storage area is abnormal.");
      }

      // Datasheet: voltage or current cycle exceeding range means invalid values
      return;
    }

    power_cycle_exceeds_range = header1 & (1 << 1);
  }

  // Parse data frame

  uint32_t voltage_coeff = this->get_24_bit_uint_(2);
  uint32_t voltage_cycle = this->get_24_bit_uint_(5);
  uint32_t current_coeff = this->get_24_bit_uint_(8);
  uint32_t current_cycle = this->get_24_bit_uint_(11);
  uint32_t power_coeff = this->get_24_bit_uint_(14);
  uint32_t power_cycle = this->get_24_bit_uint_(17);
  uint8_t adj = this->raw_data_[20];
  uint32_t cf_pulses = (this->raw_data_[21] << 8) + this->raw_data_[22];

  bool have_power = adj & 0x10;
  bool have_current = adj & 0x20;
  bool have_voltage = adj & 0x40;

  float voltage = 0.0f;
  if (have_voltage) {
    voltage = voltage_coeff / float(voltage_cycle);
    this->voltage_acc_ += voltage;
    this->voltage_counts_++;
  }

  float power = 0.0f;
  float energy = 0.0f;
  if (have_power) {
    // Datasheet: power cycle exceeding range means active power is 0
    if (!power_cycle_exceeds_range) {
      power = power_coeff / float(power_cycle);
    }
    this->power_acc_ += power;
    this->power_counts_++;

    // Add CF pulses to the total energy only if we have Power coefficient to multiply by

    if (this->cf_pulses_last_ == 0) {
      this->cf_pulses_last_ = cf_pulses;
    }

    uint32_t cf_diff;
    if (cf_pulses < this->cf_pulses_last_) {
      cf_diff = cf_pulses + (0x10000 - this->cf_pulses_last_);
    } else {
      cf_diff = cf_pulses - this->cf_pulses_last_;
    }
    this->cf_pulses_last_ = cf_pulses;

    energy = cf_diff * float(power_coeff) / 1000000.0f / 3600.0f;
    this->energy_total_ += energy;
    this->energy_total_counts_++;
  }

  float current = 0.0f;
  if (have_current) {
    if (have_voltage && !have_power) {
      // Testing has shown that when we have voltage and current but not power, that means the power is 0.
      // We report a power of 0, which in turn means we should report a current of 0.
      this->power_counts_++;
    } else if (power != 0.0f) {
      current = current_coeff / float(current_cycle);
    }
    this->current_acc_ += current;
    this->current_counts_++;
  }

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERY_VERBOSE
  {
    std::stringstream ss;
    ss << "Parsed:";
    if (have_voltage) ss << " V=" << voltage << "V";
    if (have_current) ss << " I=" << current * 1000.0f << "mA";
    if (have_power) ss << " P=" << power << "W";
    if (energy != 0.0f) ss << " E=" << energy << "kWh (" << cf_pulses << ")";
    ESP_LOGVV(TAG, "%s", ss.str().c_str());
  }
#endif
}

void CSE7766Component::update() {
  const auto publish_state = [](const char *name, sensor::Sensor *sensor, float &acc, uint32_t &counts) {
    if (counts != 0) {
      const auto avg = acc / counts;

      ESP_LOGV(TAG, "Got %s_acc=%.2f %s_counts=%" PRIu32 " %s=%.1f", name, acc, name, counts, name, avg);

      if (sensor != nullptr) {
        sensor->publish_state(avg);
      }

      acc = 0.0f;
      counts = 0;
    }
  };

  publish_state("voltage", this->voltage_sensor_, this->voltage_acc_, this->voltage_counts_);
  publish_state("current", this->current_sensor_, this->current_acc_, this->current_counts_);
  publish_state("power", this->power_sensor_, this->power_acc_, this->power_counts_);

  if (this->energy_total_counts_ != 0) {
    ESP_LOGV(TAG, "Got energy_total=%.2f energy_total_counts=%" PRIu32, this->energy_total_,
             this->energy_total_counts_);

    if (this->energy_sensor_ != nullptr) {
      this->energy_sensor_->publish_state(this->energy_total_);
    }
    this->energy_total_counts_ = 0;
  }
}

uint32_t CSE7766Component::get_24_bit_uint_(uint8_t start_index) {
  return (uint32_t(this->raw_data_[start_index]) << 16) | (uint32_t(this->raw_data_[start_index + 1]) << 8) |
         uint32_t(this->raw_data_[start_index + 2]);
}

void CSE7766Component::dump_config() {
  ESP_LOGCONFIG(TAG, "CSE7766:");
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "Voltage", this->voltage_sensor_);
  LOG_SENSOR("  ", "Current", this->current_sensor_);
  LOG_SENSOR("  ", "Power", this->power_sensor_);
  LOG_SENSOR("  ", "Energy", this->energy_sensor_);
  this->check_uart_settings(4800);
}

}  // namespace cse7766
}  // namespace esphome
