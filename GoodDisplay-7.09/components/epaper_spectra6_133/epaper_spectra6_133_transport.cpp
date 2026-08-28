#include "epaper_spectra6_133_transport.h"

/**
 * @file epaper_spectra6_133_transport.cpp
 * @brief Implements GPIO setup, SPI transactions, and BUSY polling.
 */

#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esphome/core/application.h"

namespace esphome {
namespace epaper_spectra6_133 {

static const char *const TRANSPORT_TAG = "epaper_spectra6_133.transport";

/** @brief Configures all GPIOs needed by the panel transport layer. */
bool Transport::init_gpio() {
  gpio_config_t config{};
  config.pin_bit_mask = (1ULL << this->reset_pin_) | (1ULL << this->cs_pins_[0]) | (1ULL << this->cs_pins_[1]) |
                        (1ULL << this->power_pin_);
  config.mode = GPIO_MODE_OUTPUT;
  config.pull_up_en = GPIO_PULLUP_ENABLE;

  esp_err_t err = gpio_config(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TRANSPORT_TAG, "gpio_config output failed: %s", esp_err_to_name(err));
    return false;
  }

  config = gpio_config_t{};
  config.pin_bit_mask = (1ULL << this->busy_pin_);
  config.mode = GPIO_MODE_INPUT;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.pull_up_en = GPIO_PULLUP_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;

  err = gpio_config(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TRANSPORT_TAG, "gpio_config input failed: %s", esp_err_to_name(err));
    return false;
  }

  return true;
}

/**
 * @brief Creates the SPI bus/device pair used for controller commands and pixel data.
 *
 * Repeated calls reuse the already-created device handle instead of
 * reinitialising the bus.
 */
bool Transport::init_spi() {
  if (this->spi_ != nullptr) {
    ESP_LOGD(TRANSPORT_TAG, "SPI already initialised");
    return true;
  }

  spi_bus_config_t bus_config{};
#if ESP_IDF_VERSION_MAJOR >= 5
  bus_config.data0_io_num = this->spi_data0_pin_;
  bus_config.data1_io_num = this->spi_data1_pin_;
  bus_config.data2_io_num = -1;
  bus_config.data3_io_num = -1;
#else
  bus_config.mosi_io_num = this->spi_data0_pin_;
  bus_config.miso_io_num = this->spi_data1_pin_;
#endif
  bus_config.sclk_io_num = this->spi_clk_pin_;
  bus_config.quadwp_io_num = -1;
  bus_config.quadhd_io_num = -1;
  bus_config.max_transfer_sz = SPI_MAX_TRANSFER_SIZE;
  bus_config.flags = SPICOMMON_BUSFLAG_MASTER;

  spi_device_interface_config_t device_config{};
  device_config.command_bits = 8;
  device_config.clock_speed_hz = 10 * 1000 * 1000;  // 10 MHz
  device_config.duty_cycle_pos = 128;               // 50% duty cycle
  device_config.mode = 0;
  device_config.spics_io_num = -1;  // manual CS via GPIO — the driver does not toggle CS
  device_config.queue_size = 7;
  // If hardware CS is ever enabled in the future, hold CS low briefly after
  // each transaction so slower panel input stages do not miss the final bit.
  // With manual CS (spics_io_num = -1), this field has no direct effect.
  device_config.cs_ena_posttrans = 3;

  esp_err_t err = spi_bus_initialize(this->spi_host_, &bus_config, SPI_DMA_CH_AUTO);
  if (err == ESP_ERR_INVALID_STATE) {
    ESP_LOGD(TRANSPORT_TAG, "SPI host %d already initialised, reusing bus", static_cast<int>(this->spi_host_));
  } else if (err != ESP_OK) {
    ESP_LOGE(TRANSPORT_TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
    return false;
  }

  err = spi_bus_add_device(this->spi_host_, &device_config, &this->spi_);
  if (err != ESP_OK) {
    ESP_LOGE(TRANSPORT_TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
    return false;
  }

  return true;
}

/**
 * @brief Waits until the active-low BUSY line returns to its idle high level.
 *
 * The loop feeds the watchdog periodically so long panel operations do
 * not trigger ESPHome safety resets.
 */
bool Transport::wait_busy_high(uint32_t timeout_ms) {
  const int initial_level = this->busy_level();
  if (initial_level != 0) {
    return true;
  }

  // BUSY is active-low on this panel. Poll in short slices so long
  // refreshes or power-up waits do not starve the watchdog.
  const int64_t release_wait_start = esp_timer_get_time();
  while (this->busy_level() == 0) {
    if ((esp_timer_get_time() - release_wait_start) > static_cast<int64_t>(timeout_ms) * 1000LL) {
      ESP_LOGE(TRANSPORT_TAG, "Timeout waiting for BUSY idle-high, last level=%d", this->busy_level());
      return false;
    }
    App.feed_wdt();
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  return true;
}

/** @brief Pulses the reset pin low then high using the vendor timing values. */
void Transport::hardware_reset() {
  gpio_set_level(this->reset_pin_, 0);
  this->delay_ms(20);
  gpio_set_level(this->reset_pin_, 1);
  this->delay_ms(20);
}

/** @brief Drives both controller chip-select pins to the same level. */
void Transport::set_cs_all(uint32_t level) {
  gpio_set_level(this->cs_pins_[0], level);
  gpio_set_level(this->cs_pins_[1], level);
}

/** @brief Drives one controller chip-select pin to the requested level. */
void Transport::set_cs(uint8_t index, uint32_t level) { gpio_set_level(this->cs_pins_[index], level); }

/** @brief Drives the external panel load-switch control pin. */
void Transport::set_load_switch(uint32_t level) { gpio_set_level(this->power_pin_, level); }

/** @brief Delays execution using the FreeRTOS scheduler. */
void Transport::delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

/** @brief Returns the raw logic level sampled from the BUSY pin. */
int Transport::busy_level() const { return gpio_get_level(this->busy_pin_); }

/** @brief Sends a command-only SPI transaction to the currently selected controller. */
esp_err_t Transport::write_command(uint8_t command) {
  spi_transaction_t transaction{};
  transaction.cmd = command;
  transaction.length = 0;
  transaction.tx_buffer = nullptr;
  return spi_device_transmit(this->spi_, &transaction);
}

/**
 * @brief Streams a data-only SPI payload to the currently selected controller.
 *
 * The payload is chunked as needed to stay within the configured maximum
 * transaction size while suppressing any further command phase.
 */
esp_err_t Transport::write_data(const uint8_t *data, size_t length) {
  size_t remaining = length;
  const uint8_t *cursor = data;

  while (remaining > 0) {
    // After the initial DTM command, the reference code keeps
    // clocking raw pixel bytes without another command phase.
    const size_t chunk = remaining > SPI_MAX_TRANSFER_SIZE ? SPI_MAX_TRANSFER_SIZE : remaining;
    spi_transaction_ext_t transaction{};
    transaction.command_bits = 0;
    transaction.base.length = chunk * 8;
    transaction.base.tx_buffer = cursor;
    transaction.base.flags = SPI_TRANS_VARIABLE_CMD;

    const esp_err_t err = spi_device_transmit(this->spi_, reinterpret_cast<spi_transaction_t *>(&transaction));
    if (err != ESP_OK) {
      return err;
    }

    remaining -= chunk;
    cursor += chunk;
  }

  return ESP_OK;
}

/** @brief Sends one command and its payload in a single SPI transaction. */
esp_err_t Transport::write_register(uint8_t command, const uint8_t *data, size_t length) {
  spi_transaction_t transaction{};
  transaction.cmd = command;
  transaction.length = length * 8;
  transaction.tx_buffer = data;
  transaction.rx_buffer = nullptr;
  return spi_device_transmit(this->spi_, &transaction);
}

/** @brief Sends one command and reads back a payload from the selected controller. */
esp_err_t Transport::read_register(uint8_t command, uint8_t *data, size_t length) {
  spi_transaction_t transaction{};
  transaction.cmd = command;
  transaction.length = length * 8;
  transaction.rxlength = length * 8;
  transaction.rx_buffer = data;
  return spi_device_transmit(this->spi_, &transaction);
}

}  // namespace epaper_spectra6_133
}  // namespace esphome
