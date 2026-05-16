#pragma once

#include "SPI.h"
#include "driver/gpio.h"
#include "esphome/components/display/display_buffer.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace it8951_reterminal_e1003 {

// Pin definitions based on Seeed Setup522_Seeed_reTerminal_E1003.h
static const gpio_num_t IT8951_PIN_SCK = GPIO_NUM_7;
static const gpio_num_t IT8951_PIN_MISO = GPIO_NUM_8;
static const gpio_num_t IT8951_PIN_MOSI = GPIO_NUM_9;
static const gpio_num_t IT8951_PIN_CS = GPIO_NUM_10;
static const gpio_num_t IT8951_PIN_BUSY = GPIO_NUM_13;
static const gpio_num_t IT8951_PIN_EN = GPIO_NUM_11;
static const gpio_num_t IT8951_PIN_RST = GPIO_NUM_12;
static const gpio_num_t IT8951_PIN_ITE_EN = GPIO_NUM_21;

#define IT8951_TCON_SYS_RUN      0x0001
#define IT8951_TCON_STANDBY      0x0002
#define IT8951_TCON_SLEEP        0x0003
#define IT8951_TCON_REG_RD       0x0010
#define IT8951_TCON_REG_WR       0x0011
#define IT8951_TCON_LD_IMG       0x0020
#define IT8951_TCON_LD_IMG_AREA  0x0021
#define IT8951_TCON_LD_IMG_END   0x0022

#define USDEF_I80_CMD_DPY_AREA     0x0034
#define USDEF_I80_CMD_GET_DEV_INFO 0x0302
#define USDEF_I80_CMD_VCOM         0x0039
#define USDEF_I80_CMD_TEMP         0x0040

#define IT8951_4BPP 2
#define IT8951_8BPP 3

#define IT8951_LDIMG_L_ENDIAN 0

#define I80CPCR 0x0004
#define LISAR 0x0208
#define LUTAFSR 0x1224
#define UP1SR 0x1138
#define BGVR 0x1250

typedef struct {
  uint16_t panel_width;
  uint16_t panel_height;
  uint16_t img_buf_addr_l;
  uint16_t img_buf_addr_h;
  uint16_t fw_version[8];
  uint16_t lut_version[8];
} IT8951DevInfo;

class IT8951ReTerminalE1003Display : public display::DisplayBuffer {
 public:
  void set_vcom(uint16_t vcom) { this->vcom_ = vcom; }
  void set_temperature(int temp_celsius) { this->temperature_ = static_cast<int8_t>(temp_celsius); }

  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  display::DisplayType get_display_type() override { return display::DisplayType::DISPLAY_TYPE_GRAYSCALE; }

 protected:
  int get_width_internal() override { return 1872; }
  int get_height_internal() override { return 1404; }
  void draw_absolute_pixel_internal(int x, int y, Color color) override;

  void spi_send_word_(uint16_t word);
  uint16_t spi_recv_word_();
  void lcd_write_cmd_code_(uint16_t cmd);
  void lcd_write_data_(uint16_t data);
  void lcd_write_n_data_(uint16_t *buf, uint32_t word_count);
  void lcd_write_framebuffer_4bpp_(uint16_t *buf, uint16_t width_in_words, uint16_t height);
  void lcd_write_framebuffer_1bpp_(uint16_t width, uint16_t height);
  uint16_t lcd_read_data_();
  void lcd_read_n_data_(uint16_t *buf, uint32_t word_count);
  void lcd_wait_for_ready_();
  void lcd_sys_run_();
  void hardware_reset_();
  void power_cycle_();
  void wait_for_display_ready_();
  void log_framebuffer_stats_();
  uint32_t count_non_white_bytes_();
  bool framebuffer_is_binary_();
  void draw_driver_test_pattern_();
  uint8_t get_pixel_nibble_(uint16_t x, uint16_t y);

  void lcd_send_cmd_arg_(uint16_t cmd, uint16_t *args, uint16_t num_args);
  uint16_t it8951_read_reg_(uint16_t addr);
  void it8951_write_reg_(uint16_t addr, uint16_t val);
  void get_it8951_system_info_();
  void set_img_buf_base_addr_(uint32_t addr);
  void it8951_load_img_area_start_(uint16_t endian, uint16_t pix_fmt, uint16_t rotate, uint16_t x, uint16_t y,
                                   uint16_t w, uint16_t h);
  void it8951_display_area_(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t mode);
  void it8951_display_area_1bpp_(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t mode, uint8_t bg_gray,
                                 uint8_t fg_gray);
  void log_dev_info_words_(const char *label);
  bool has_valid_dev_info_() const;
  void write_vcom_(uint16_t selector, uint16_t value);
  bool probe_controller_(const char *label, bool send_sys_run, int vcom_selector);

  IT8951DevInfo dev_info_{};
  uint32_t img_buf_addr_{0};
  uint16_t vcom_{1400};
  uint32_t spi_frequency_{1000000};
  uint16_t vcom_write_selector_{0};
  const char *probe_path_{nullptr};

  uint8_t *framebuffer_{nullptr};
  const char *fail_reason_{nullptr};
  uint16_t probe_vcom_{0};
  bool first_update_{true};
  bool it8951_sleeping_{false};
  int8_t temperature_{23};
  uint32_t spi_read_frequency_{1000000};
};

}  // namespace it8951_reterminal_e1003
}  // namespace esphome
