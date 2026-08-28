#pragma once

/**
 * @file epaper_spectra6_133_transport.h
 * @brief Low-level GPIO and SPI transport for the GDEP133C02 controller pair.
 */

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

#include "epaper_spectra6_133_constants.h"

namespace esphome {
namespace epaper_spectra6_133 {

/**
 * Low-level GPIO and SPI access for the two-chip GDEP133C02 panel.
 *
 * The panel shares one SPI peripheral but exposes two manual chip-select
 * lines, so the transport intentionally bypasses ESPHome's higher-level
 * SPI helpers and talks to ESP-IDF's master API directly.
 */
class Transport {
 public:
  /** @brief Sets the SPI host peripheral used by the panel transport. */
  void set_spi_host(int host) { this->spi_host_ = static_cast<spi_host_device_t>(host); }
  /** @brief Sets the chip-select pin for controller half 0. */
  void set_cs0_pin(int pin) { this->cs_pins_[0] = static_cast<gpio_num_t>(pin); }
  /** @brief Sets the chip-select pin for controller half 1. */
  void set_cs1_pin(int pin) { this->cs_pins_[1] = static_cast<gpio_num_t>(pin); }
  /** @brief Sets the SPI clock pin. */
  void set_clk_pin(int pin) { this->spi_clk_pin_ = static_cast<gpio_num_t>(pin); }
  /** @brief Sets the DATA0 signal pin. */
  void set_data0_pin(int pin) { this->spi_data0_pin_ = static_cast<gpio_num_t>(pin); }
  /** @brief Sets the DATA1 signal pin. */
  void set_data1_pin(int pin) { this->spi_data1_pin_ = static_cast<gpio_num_t>(pin); }
  /** @brief Sets the active-low BUSY input pin. */
  void set_busy_pin(int pin) { this->busy_pin_ = static_cast<gpio_num_t>(pin); }
  /** @brief Sets the hardware reset pin. */
  void set_reset_pin(int pin) { this->reset_pin_ = static_cast<gpio_num_t>(pin); }
  /** @brief Sets the external load-switch control pin. */
  void set_power_pin(int pin) { this->power_pin_ = static_cast<gpio_num_t>(pin); }

  /** @brief Returns the configured SPI host. */
  spi_host_device_t spi_host() const { return this->spi_host_; }
  /** @brief Returns the configured chip-select pin for controller half 0. */
  gpio_num_t cs0_pin() const { return this->cs_pins_[0]; }
  /** @brief Returns the configured chip-select pin for controller half 1. */
  gpio_num_t cs1_pin() const { return this->cs_pins_[1]; }
  /** @brief Returns the configured SPI clock pin. */
  gpio_num_t clk_pin() const { return this->spi_clk_pin_; }
  /** @brief Returns the configured DATA0 pin. */
  gpio_num_t data0_pin() const { return this->spi_data0_pin_; }
  /** @brief Returns the configured DATA1 pin. */
  gpio_num_t data1_pin() const { return this->spi_data1_pin_; }
  /** @brief Returns the configured BUSY pin. */
  gpio_num_t busy_pin() const { return this->busy_pin_; }
  /** @brief Returns the configured reset pin. */
  gpio_num_t reset_pin() const { return this->reset_pin_; }
  /** @brief Returns the configured load-switch pin. */
  gpio_num_t power_pin() const { return this->power_pin_; }

  /** @brief Configures reset, chip-select, load-switch, and busy GPIO directions. */
  bool init_gpio();
  /** @brief Creates or reuses the SPI bus/device used for commands and pixel payloads. */
  bool init_spi();
  /**
   * @brief Waits until the controller releases the active-low BUSY line.
   *
   * @param timeout_ms Maximum time to wait before reporting a transport error.
   */
  bool wait_busy_high(uint32_t timeout_ms = 8000);

  /** @brief Pulses the reset line according to the vendor timing diagram. */
  void hardware_reset();
  /** @brief Drives both chip-select outputs to the same logic level. */
  void set_cs_all(uint32_t level);
  /** @brief Drives one chip-select output.
   *  @param index Controller half index, either 0 or 1.
   *  @param level GPIO output level.
   */
  void set_cs(uint8_t index, uint32_t level);
  /** @brief Enables or disables the external load switch feeding the panel. */
  void set_load_switch(uint32_t level);
  /** @brief Sleeps for the given number of milliseconds using FreeRTOS timing. */
  void delay_ms(uint32_t ms);

  /** @brief Reads the raw logic level currently present on the BUSY pin. */
  int busy_level() const;

  /** @brief Sends a command byte without payload data. */
  esp_err_t write_command(uint8_t command);
  /**
   * @brief Sends data bytes after a previously issued command phase.
   *
   * Large transfers are split into multiple transactions to stay within
   * the SPI driver's configured maximum transfer size.
   */
  esp_err_t write_data(const uint8_t *data, size_t length);
  /** @brief Sends one command together with its payload in a single transaction. */
  esp_err_t write_register(uint8_t command, const uint8_t *data, size_t length);
  /** @brief Reads a response payload produced by one controller command. */
  esp_err_t read_register(uint8_t command, uint8_t *data, size_t length);

 private:
  gpio_num_t cs_pins_[2]{GPIO_NUM_NC, GPIO_NUM_NC};
  gpio_num_t spi_clk_pin_{GPIO_NUM_NC};
  gpio_num_t spi_data0_pin_{GPIO_NUM_NC};
  gpio_num_t spi_data1_pin_{GPIO_NUM_NC};
  gpio_num_t busy_pin_{GPIO_NUM_NC};
  gpio_num_t reset_pin_{GPIO_NUM_NC};
  gpio_num_t power_pin_{GPIO_NUM_NC};
  spi_host_device_t spi_host_{SPI3_HOST};
  spi_device_handle_t spi_{nullptr};
};

}  // namespace epaper_spectra6_133
}  // namespace esphome
