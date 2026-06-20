#include "it8951_reterminal_e1003.h"

#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome {
namespace it8951_reterminal_e1003 {

static const char *const TAG = "it8951_e1003";
static const uint32_t IT8951_SPI_PROBE_FREQUENCY = 1000000;
static const uint32_t IT8951_SPI_WRITE_FREQUENCY = 20000000;  // IT8951DX max is 24 MHz; 20 MHz (80/4) is the nearest safe ESP32-S3 divisor
static const uint32_t IT8951_SPI_READ_FREQUENCY = 4000000;

void IT8951ReTerminalE1003Display::spi_send_word_(uint16_t word) { SPI.transfer16(word); }

uint16_t IT8951ReTerminalE1003Display::spi_recv_word_() { return SPI.transfer16(0); }

void IT8951ReTerminalE1003Display::lcd_wait_for_ready_() {
  const uint32_t start = millis();
  while (digitalRead(IT8951_PIN_BUSY) == LOW) {
    if (millis() - start > 3000) {
      ESP_LOGE(TAG, "HRDY timeout!");
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void IT8951ReTerminalE1003Display::hardware_reset_() {
  digitalWrite(IT8951_PIN_CS, HIGH);
  digitalWrite(IT8951_PIN_RST, HIGH);
  digitalWrite(IT8951_PIN_EN, HIGH);
  digitalWrite(IT8951_PIN_ITE_EN, HIGH);
  delay(50);
  digitalWrite(IT8951_PIN_RST, LOW);
  delay(10);
  digitalWrite(IT8951_PIN_RST, HIGH);
  delay(10);
}

void IT8951ReTerminalE1003Display::power_cycle_() {
  digitalWrite(IT8951_PIN_CS, HIGH);
  digitalWrite(IT8951_PIN_RST, HIGH);
  digitalWrite(IT8951_PIN_EN, LOW);
  digitalWrite(IT8951_PIN_ITE_EN, LOW);
  delay(100);
  digitalWrite(IT8951_PIN_EN, HIGH);
  digitalWrite(IT8951_PIN_ITE_EN, HIGH);
  delay(500);
  this->hardware_reset_();
  delay(1500);
}

void IT8951ReTerminalE1003Display::lcd_write_cmd_code_(uint16_t cmd) {
  digitalWrite(IT8951_PIN_CS, LOW);
  SPI.beginTransaction(SPISettings(this->spi_frequency_, MSBFIRST, SPI_MODE0));
  this->lcd_wait_for_ready_();
  this->spi_send_word_(0x6000);
  this->lcd_wait_for_ready_();
  this->spi_send_word_(cmd);
  SPI.endTransaction();
  digitalWrite(IT8951_PIN_CS, HIGH);
}

void IT8951ReTerminalE1003Display::lcd_write_data_(uint16_t data) {
  digitalWrite(IT8951_PIN_CS, LOW);
  SPI.beginTransaction(SPISettings(this->spi_frequency_, MSBFIRST, SPI_MODE0));
  this->lcd_wait_for_ready_();
  this->spi_send_word_(0x0000);
  this->lcd_wait_for_ready_();
  this->spi_send_word_(data);
  SPI.endTransaction();
  digitalWrite(IT8951_PIN_CS, HIGH);
}

void IT8951ReTerminalE1003Display::lcd_write_n_data_(uint16_t *buf, uint32_t word_count) {
  digitalWrite(IT8951_PIN_CS, LOW);
  SPI.beginTransaction(SPISettings(this->spi_frequency_, MSBFIRST, SPI_MODE0));
  this->lcd_wait_for_ready_();
  this->spi_send_word_(0x0000);
  this->lcd_wait_for_ready_();
  for (uint32_t i = 0; i < word_count; i++) {
    this->spi_send_word_(buf[i]);
  }
  SPI.endTransaction();
  digitalWrite(IT8951_PIN_CS, HIGH);
}

void IT8951ReTerminalE1003Display::lcd_write_framebuffer_4bpp_(uint16_t *buf, uint16_t width_in_words,
                                                               uint16_t height) {
  // One row at 1872px / 4bpp is 936 bytes. Sending a whole row at once is much
  // faster than hundreds of thousands of per-word SPI.transfer16() calls.
  uint8_t row_buffer[936];
  const uint32_t row_size_bytes = uint32_t(width_in_words) * 2;
  if (row_size_bytes > sizeof(row_buffer)) {
    ESP_LOGE(TAG, "Row buffer too small for %u-byte transfer", static_cast<unsigned>(row_size_bytes));
    return;
  }

  digitalWrite(IT8951_PIN_CS, LOW);
  SPI.beginTransaction(SPISettings(this->spi_frequency_, MSBFIRST, SPI_MODE0));
  this->lcd_wait_for_ready_();
  this->spi_send_word_(0x0000);
  this->lcd_wait_for_ready_();

  for (uint16_t y = 0; y < height; y++) {
    const uint32_t row_start = uint32_t(y) * width_in_words;
    for (uint16_t x = 0; x < width_in_words; x++) {
      const uint16_t word = buf[row_start + x];
      const uint32_t byte_index = uint32_t(x) * 2;
      row_buffer[byte_index] = static_cast<uint8_t>(word >> 8);
      row_buffer[byte_index + 1] = static_cast<uint8_t>(word & 0xFF);
    }
    SPI.writeBytes(row_buffer, row_size_bytes);
    if ((y & 0x07) == 0) {
      App.feed_wdt();
    }
  }

  SPI.endTransaction();
  digitalWrite(IT8951_PIN_CS, HIGH);
}

void IT8951ReTerminalE1003Display::lcd_write_framebuffer_1bpp_(uint16_t width, uint16_t height) {
  uint16_t row_words[117];
  uint8_t row_buffer[234];
  const uint16_t width_in_words = width / 16;
  const uint32_t row_size_bytes = uint32_t(width_in_words) * 2;

  if (width_in_words > (sizeof(row_words) / sizeof(row_words[0])) || row_size_bytes > sizeof(row_buffer)) {
    ESP_LOGE(TAG, "1bpp row buffer too small for %u-byte transfer", static_cast<unsigned>(row_size_bytes));
    return;
  }

  digitalWrite(IT8951_PIN_CS, LOW);
  SPI.beginTransaction(SPISettings(this->spi_frequency_, MSBFIRST, SPI_MODE0));
  this->lcd_wait_for_ready_();
  this->spi_send_word_(0x0000);
  this->lcd_wait_for_ready_();

  for (uint16_t y = 0; y < height; y++) {
    memset(row_words, 0x00, row_size_bytes);

    for (uint16_t x = 0; x < width; x++) {
      const uint8_t nibble = this->get_pixel_nibble_(x, y);
      if (nibble <= 0x07) {
        row_words[x / 16] |= uint16_t(1 << (x & 0x0F));
      }
    }

    for (uint16_t i = 0; i < width_in_words; i++) {
      const uint16_t word = row_words[i];
      const uint32_t byte_index = uint32_t(i) * 2;
      row_buffer[byte_index] = static_cast<uint8_t>(word >> 8);
      row_buffer[byte_index + 1] = static_cast<uint8_t>(word & 0xFF);
    }

    SPI.writeBytes(row_buffer, row_size_bytes);
    if ((y & 0x07) == 0) {
      App.feed_wdt();
    }
  }

  SPI.endTransaction();
  digitalWrite(IT8951_PIN_CS, HIGH);
}

uint16_t IT8951ReTerminalE1003Display::lcd_read_data_() {
  digitalWrite(IT8951_PIN_CS, LOW);
  SPI.beginTransaction(SPISettings(this->spi_read_frequency_, MSBFIRST, SPI_MODE0));
  this->lcd_wait_for_ready_();
  this->spi_send_word_(0x1000);
  this->spi_recv_word_();
  this->lcd_wait_for_ready_();
  const uint16_t data = this->spi_recv_word_();
  SPI.endTransaction();
  digitalWrite(IT8951_PIN_CS, HIGH);
  return data;
}

void IT8951ReTerminalE1003Display::lcd_read_n_data_(uint16_t *buf, uint32_t word_count) {
  digitalWrite(IT8951_PIN_CS, LOW);
  SPI.beginTransaction(SPISettings(this->spi_read_frequency_, MSBFIRST, SPI_MODE0));
  this->lcd_wait_for_ready_();
  this->spi_send_word_(0x1000);
  this->lcd_wait_for_ready_();
  this->spi_recv_word_();
  this->lcd_wait_for_ready_();
  for (uint32_t i = 0; i < word_count; i++) {
    buf[i] = this->spi_recv_word_();
  }
  SPI.endTransaction();
  digitalWrite(IT8951_PIN_CS, HIGH);
}

void IT8951ReTerminalE1003Display::lcd_sys_run_() { this->lcd_write_cmd_code_(IT8951_TCON_SYS_RUN); }

void IT8951ReTerminalE1003Display::wait_for_display_ready_() {
  const uint32_t start = millis();
  while (this->it8951_read_reg_(LUTAFSR) != 0) {
    if (millis() - start > 30000) {
      ESP_LOGW(TAG, "Display-ready timeout while waiting for LUTAFSR to clear");
      break;
    }
    App.feed_wdt();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void IT8951ReTerminalE1003Display::write_vcom_(uint16_t selector, uint16_t value) {
  this->lcd_write_cmd_code_(USDEF_I80_CMD_VCOM);
  this->lcd_write_data_(selector);
  this->lcd_write_data_(value);
}

bool IT8951ReTerminalE1003Display::has_valid_dev_info_() const {
  return this->dev_info_.panel_width > 0 && this->dev_info_.panel_width < 10000 &&
         this->dev_info_.panel_height > 0 && this->dev_info_.panel_height < 10000;
}

void IT8951ReTerminalE1003Display::log_dev_info_words_(const char *label) {
  uint16_t *raw = reinterpret_cast<uint16_t *>(&this->dev_info_);
  ESP_LOGI(TAG, "[%s] DevInfo raw [W=%u H=%u BufL=0x%04X BufH=0x%04X]", label, raw[0], raw[1], raw[2], raw[3]);
  ESP_LOGI(TAG, "[%s] DevInfo FW: %02X %02X %02X %02X %02X %02X %02X %02X", label, raw[4], raw[5], raw[6], raw[7],
           raw[8], raw[9], raw[10], raw[11]);
  ESP_LOGI(TAG, "[%s] DevInfo LUT: %02X %02X %02X %02X %02X %02X %02X %02X", label, raw[12], raw[13], raw[14],
           raw[15], raw[16], raw[17], raw[18], raw[19]);
}

bool IT8951ReTerminalE1003Display::probe_controller_(const char *label, bool send_sys_run, int vcom_selector) {
  this->probe_path_ = label;
  this->probe_vcom_ = 0;
  memset(&this->dev_info_, 0, sizeof(this->dev_info_));

  if (send_sys_run) {
    ESP_LOGI(TAG, "[%s] Sending SYS_RUN wake command", label);
    this->lcd_sys_run_();
    delay(10);
  }

  if (vcom_selector > 0) {
    ESP_LOGI(TAG, "[%s] Writing VCOM=%u with selector 0x%04X", label, this->vcom_, vcom_selector);
    this->write_vcom_(vcom_selector, this->vcom_);
    this->lcd_write_cmd_code_(USDEF_I80_CMD_VCOM);
    this->lcd_write_data_(0x0000);
    this->probe_vcom_ = this->lcd_read_data_();
    ESP_LOGI(TAG, "[%s] VCOM read-back: %u (0x%04X)", label, this->probe_vcom_, this->probe_vcom_);
    if (this->probe_vcom_ == this->vcom_) {
      this->vcom_write_selector_ = vcom_selector;
    }
  }

  this->get_it8951_system_info_();
  this->log_dev_info_words_(label);
  return this->has_valid_dev_info_();
}

void IT8951ReTerminalE1003Display::setup() {
  ESP_LOGCONFIG(TAG, "Setting up IT8951 reTerminal E1003...");

  pinMode(IT8951_PIN_CS, OUTPUT);
  pinMode(IT8951_PIN_EN, OUTPUT);
  pinMode(IT8951_PIN_ITE_EN, OUTPUT);
  pinMode(IT8951_PIN_RST, OUTPUT);
  pinMode(IT8951_PIN_BUSY, INPUT);

  digitalWrite(IT8951_PIN_CS, HIGH);
  digitalWrite(IT8951_PIN_EN, HIGH);
  digitalWrite(IT8951_PIN_ITE_EN, HIGH);
  digitalWrite(IT8951_PIN_RST, HIGH);
  ESP_LOGI(TAG, "ENABLE HIGH, ITE_ENABLE HIGH, RESET HIGH, CS HIGH");

  this->spi_frequency_ = IT8951_SPI_PROBE_FREQUENCY;
  SPI.begin(IT8951_PIN_SCK, IT8951_PIN_MISO, IT8951_PIN_MOSI, -1);
  ESP_LOGI(TAG, "SPI bus initialized at %u Hz probe speed", this->spi_frequency_);

  struct ProbeAttempt {
    const char *label;
    bool send_sys_run;
    int vcom_selector;
  };
  static const ProbeAttempt attempts[] = {
      {"cold read", false, 0},
      {"wake then read", true, 0},
      {"wake + VCOM 0x0001", true, 0x0001},
      {"wake + VCOM 0x0002", true, 0x0002},
  };

  bool found_device = false;
  for (size_t i = 0; i < sizeof(attempts) / sizeof(attempts[0]); i++) {
    ESP_LOGI(TAG, "Probe attempt %u: %s", static_cast<unsigned>(i + 1), attempts[i].label);
    this->power_cycle_();
    ESP_LOGI(TAG, "[%s] Power cycle complete, HRDY=%s", attempts[i].label,
             digitalRead(IT8951_PIN_BUSY) ? "HIGH" : "LOW");
    this->lcd_wait_for_ready_();
    if (this->probe_controller_(attempts[i].label, attempts[i].send_sys_run, attempts[i].vcom_selector)) {
      found_device = true;
      break;
    }
  }

  if (!found_device) {
    ESP_LOGW(TAG, "No valid device info was returned during E1003 probe attempts.");
    this->fail_reason_ = "SPI communication failed - IT8951 never returned valid device info";
    this->mark_failed();
    return;
  }

  if (this->vcom_write_selector_ == 0) {
    ESP_LOGI(TAG, "Panel answered before VCOM was verified, trying preferred selector 0x0002");
    this->write_vcom_(0x0002, this->vcom_);
    this->lcd_write_cmd_code_(USDEF_I80_CMD_VCOM);
    this->lcd_write_data_(0x0000);
    this->probe_vcom_ = this->lcd_read_data_();
    if (this->probe_vcom_ == this->vcom_) {
      this->vcom_write_selector_ = 0x0002;
    } else {
      ESP_LOGW(TAG, "Selector 0x0002 read-back was %u, trying selector 0x0001", this->probe_vcom_);
      this->write_vcom_(0x0001, this->vcom_);
      this->lcd_write_cmd_code_(USDEF_I80_CMD_VCOM);
      this->lcd_write_data_(0x0000);
      this->probe_vcom_ = this->lcd_read_data_();
      if (this->probe_vcom_ == this->vcom_) {
        this->vcom_write_selector_ = 0x0001;
      }
    }
    ESP_LOGI(TAG, "Post-detect VCOM read-back: %u (0x%04X)", this->probe_vcom_, this->probe_vcom_);
  }

  this->img_buf_addr_ = (uint32_t(this->dev_info_.img_buf_addr_h) << 16) | this->dev_info_.img_buf_addr_l;
  ESP_LOGI(TAG, "Panel: %dx%d, ImgBuf: 0x%08X", this->dev_info_.panel_width, this->dev_info_.panel_height,
           this->img_buf_addr_);

  this->it8951_write_reg_(I80CPCR, 0x0001);

  this->spi_frequency_ = IT8951_SPI_WRITE_FREQUENCY;
  this->spi_read_frequency_ = IT8951_SPI_READ_FREQUENCY;
  ESP_LOGI(TAG, "Switching SPI to %u Hz write / %u Hz read", this->spi_frequency_, this->spi_read_frequency_);

  const uint32_t buffer_size = (uint32_t(this->get_width_internal()) * this->get_height_internal()) / 2;
  this->framebuffer_ = static_cast<uint8_t *>(heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM));
  if (this->framebuffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %u bytes in PSRAM!", buffer_size);
    this->fail_reason_ = "PSRAM allocation failed";
    this->mark_failed();
    return;
  }
  memset(this->framebuffer_, 0xFF, buffer_size);

  // Carte SD sur le bus SPI partage (CS=14). Alimentation SD = GPIO39 (SD_EN).
  pinMode(IT8951_PIN_SD_EN, OUTPUT);
  digitalWrite(IT8951_PIN_SD_EN, HIGH);
  delay(10);
  this->sd_ok_ = this->sd_.begin(SdSpiConfig(IT8951_PIN_SD_CS, SHARED_SPI, SD_SCK_MHZ(10), &SPI));
  ESP_LOGCONFIG(TAG, "SD card: %s", this->sd_ok_ ? "OK" : "absente/echec init");

  ESP_LOGCONFIG(TAG, "IT8951 reTerminal E1003 initialization complete");
}

void IT8951ReTerminalE1003Display::log_framebuffer_stats_() {
  if (this->framebuffer_ == nullptr) {
    ESP_LOGW(TAG, "Framebuffer stats requested before allocation");
    return;
  }

  const uint32_t buffer_size = (uint32_t(this->get_width_internal()) * this->get_height_internal()) / 2;
  uint32_t non_white = 0;
  uint32_t pure_black = 0;
  for (uint32_t i = 0; i < buffer_size; i++) {
    if (this->framebuffer_[i] != 0xFF) {
      non_white++;
    }
    if (this->framebuffer_[i] == 0x00) {
      pure_black++;
    }
  }

  const uint32_t sample_pos = ((100 + 100 * this->get_width_internal()) / 2);
  const uint8_t sample = sample_pos < buffer_size ? this->framebuffer_[sample_pos] : 0xFF;
  ESP_LOGD(TAG, "Framebuffer stats: non_white=%u pure_black=%u sample[100,100]=0x%02X", non_white, pure_black, sample);
}

uint32_t IT8951ReTerminalE1003Display::count_non_white_bytes_() {
  if (this->framebuffer_ == nullptr) {
    return 0;
  }
  const uint32_t buffer_size = (uint32_t(this->get_width_internal()) * this->get_height_internal()) / 2;
  uint32_t non_white = 0;
  for (uint32_t i = 0; i < buffer_size; i++) {
    if (this->framebuffer_[i] != 0xFF) {
      non_white++;
    }
  }
  return non_white;
}

bool IT8951ReTerminalE1003Display::framebuffer_is_binary_() {
  if (this->framebuffer_ == nullptr) {
    return false;
  }

  const uint32_t buffer_size = (uint32_t(this->get_width_internal()) * this->get_height_internal()) / 2;
  for (uint32_t i = 0; i < buffer_size; i++) {
    const uint8_t hi = this->framebuffer_[i] >> 4;
    const uint8_t lo = this->framebuffer_[i] & 0x0F;
    if ((hi != 0x00 && hi != 0x0F) || (lo != 0x00 && lo != 0x0F)) {
      return false;
    }
  }
  return true;
}

void IT8951ReTerminalE1003Display::draw_driver_test_pattern_() {
  if (this->framebuffer_ == nullptr) {
    return;
  }

  ESP_LOGW(TAG, "Writer callback left the framebuffer blank, drawing fallback driver test pattern");

  for (int x = 20; x < 1852; x++) {
    this->draw_absolute_pixel_internal(x, 20, Color(0, 0, 0));
    this->draw_absolute_pixel_internal(x, 1383, Color(0, 0, 0));
  }
  for (int y = 20; y < 1384; y++) {
    this->draw_absolute_pixel_internal(20, y, Color(0, 0, 0));
    this->draw_absolute_pixel_internal(1851, y, Color(0, 0, 0));
  }

  for (int y = 120; y < 360; y++) {
    for (int x = 120; x < 360; x++) {
      this->draw_absolute_pixel_internal(x, y, Color(0, 0, 0));
    }
  }

  for (int i = 0; i < 500; i++) {
    this->draw_absolute_pixel_internal(500 + i, 500 + i, Color(0, 0, 0));
    this->draw_absolute_pixel_internal(1371 - i, 500 + i, Color(0, 0, 0));
  }
}

void IT8951ReTerminalE1003Display::update() { this->full_refresh(); }

void IT8951ReTerminalE1003Display::wake_panel_() {
  if (this->it8951_sleeping_) {
    ESP_LOGD(TAG, "Waking IT8951 from inter-refresh sleep");
    digitalWrite(IT8951_PIN_EN, HIGH);
    delay(10);  // allow TPS22916 power switch to close before SPI
    this->lcd_sys_run_();
    this->it8951_sleeping_ = false;
  }
}

void IT8951ReTerminalE1003Display::sleep_panel_() {
  this->lcd_write_cmd_code_(IT8951_TCON_SLEEP);
  digitalWrite(IT8951_PIN_EN, LOW);
  this->it8951_sleeping_ = true;
  ESP_LOGD(TAG, "IT8951 sleeping, EPD_Drive power cut");
}

void IT8951ReTerminalE1003Display::full_refresh() {
  if (this->framebuffer_ == nullptr) {
    ESP_LOGW(TAG, "Skipping update because the framebuffer is not available");
    return;
  }

  this->wake_panel_();

  this->do_update_();
  this->log_framebuffer_stats_();
  if (this->count_non_white_bytes_() == 0) {
    this->draw_driver_test_pattern_();
    this->log_framebuffer_stats_();
  }

  ESP_LOGD(TAG, "Transferring full image to IT8951...");

  const uint16_t w = this->get_width_internal();
  const uint16_t h = this->get_height_internal();
  const uint16_t width_in_words = (w + 3) / 4;
  const bool use_1bpp = this->framebuffer_is_binary_();

  this->wait_for_display_ready_();

  this->lcd_write_cmd_code_(USDEF_I80_CMD_TEMP);
  this->lcd_write_data_(0x0001);  // Force Set from host
  this->lcd_write_data_(static_cast<uint16_t>(this->temperature_));
  ESP_LOGD(TAG, "Temperature set to %d°C for waveform selection", this->temperature_);

  this->it8951_write_reg_(UP1SR + 2, this->it8951_read_reg_(UP1SR + 2) & ~(1 << 2));
  this->set_img_buf_base_addr_(this->img_buf_addr_);
  if (use_1bpp) {
    const uint16_t one_bpp_width_bytes = w / 8;
    ESP_LOGD(TAG, "Using sharp 1bpp upload path");
    this->it8951_load_img_area_start_(IT8951_LDIMG_L_ENDIAN, IT8951_8BPP, 0, 0, 0, one_bpp_width_bytes, h);
    this->lcd_write_framebuffer_1bpp_(w, h);
  } else {
    ESP_LOGD(TAG, "Using 4bpp grayscale upload path");
    this->it8951_load_img_area_start_(IT8951_LDIMG_L_ENDIAN, IT8951_4BPP, 0, 0, 0, w, h);
    this->lcd_write_framebuffer_4bpp_(reinterpret_cast<uint16_t *>(this->framebuffer_), width_in_words, h);
  }

  this->lcd_write_cmd_code_(IT8951_TCON_LD_IMG_END);

  // INIT pass (mode 0) fully discharges residual pixel state from the previous image,
  // eliminating ghosting before the grayscale render.
  ESP_LOGD(TAG, "Issuing INIT clear pass (mode 0)");
  this->it8951_display_area_(0, 0, w, h, 0);
  this->wait_for_display_ready_();

  ESP_LOGD(TAG, "Issuing GC16 display refresh (mode 2)");
  if (use_1bpp) {
    this->it8951_display_area_1bpp_(0, 0, w, h, 2, 0xFF, 0x00);
  } else {
    this->it8951_display_area_(0, 0, w, h, 2);
    this->wait_for_display_ready_();
  }
  this->first_update_ = false;

  this->sleep_panel_();
  this->partials_since_full_ = 0;
}

// Upload only a sub-rectangle of the framebuffer in 4bpp. x and w MUST be
// multiples of 4 (4 pixels per 16-bit word). Byte order matches
// lcd_write_framebuffer_4bpp_ (MSB-first per word).
void IT8951ReTerminalE1003Display::lcd_write_framebuffer_4bpp_area_(uint16_t x, uint16_t y, uint16_t w,
                                                                    uint16_t h) {
  const uint16_t words = w / 4;
  const uint32_t row_size_bytes = uint32_t(words) * 2;
  uint8_t row_buffer[936];
  if (row_size_bytes > sizeof(row_buffer)) {
    ESP_LOGE(TAG, "Area row buffer too small for %u-byte transfer", static_cast<unsigned>(row_size_bytes));
    return;
  }
  const uint32_t fb_row_bytes = uint32_t(this->get_width_internal()) / 2;
  const uint32_t x_byte = uint32_t(x) / 2;

  digitalWrite(IT8951_PIN_CS, LOW);
  SPI.beginTransaction(SPISettings(this->spi_frequency_, MSBFIRST, SPI_MODE0));
  this->lcd_wait_for_ready_();
  this->spi_send_word_(0x0000);
  this->lcd_wait_for_ready_();

  for (uint16_t yy = 0; yy < h; yy++) {
    const uint8_t *src = this->framebuffer_ + (uint32_t(y) + yy) * fb_row_bytes + x_byte;
    for (uint16_t i = 0; i < words; i++) {
      const uint8_t b0 = src[uint32_t(i) * 2];
      const uint8_t b1 = src[uint32_t(i) * 2 + 1];
      row_buffer[uint32_t(i) * 2] = b1;
      row_buffer[uint32_t(i) * 2 + 1] = b0;
    }
    SPI.writeBytes(row_buffer, row_size_bytes);
    if ((yy & 0x07) == 0) {
      App.feed_wdt();
    }
  }

  SPI.endTransaction();
  digitalWrite(IT8951_PIN_CS, HIGH);
}

void IT8951ReTerminalE1003Display::render_framebuffer() {
  if (this->framebuffer_ == nullptr) {
    return;
  }
  // Runs the display lambda once. Heavy on PSRAM (~seconds); do this ONCE then
  // call flush_zone() for each zone rather than re-rendering per zone.
  this->do_update_();
}

void IT8951ReTerminalE1003Display::refresh_zone(int lx, int ly, int lw, int lh, int mode) {
  this->render_framebuffer();
  this->flush_zone(lx, ly, lw, lh, mode);
}

void IT8951ReTerminalE1003Display::flush_zone(int lx, int ly, int lw, int lh, int mode) {
  if (this->framebuffer_ == nullptr || lw <= 0 || lh <= 0) {
    return;
  }
  const int panel_w = this->get_width_internal();
  const int panel_h = this->get_height_internal();

  // The framebuffer is mirrored in X (see draw_absolute_pixel_internal):
  // logical column lx maps to panel column panel_w - 1 - lx. Convert the
  // logical rectangle to panel coordinates, then align x/w to 4 px (4bpp).
  int px = panel_w - lx - lw;
  int pend = px + lw;
  if (px < 0) px = 0;
  if (pend > panel_w) pend = panel_w;
  px &= ~0x03;
  pend = (pend + 3) & ~0x03;
  const int pw = pend - px;

  int y = ly;
  int h = lh;
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (y + h > panel_h) {
    h = panel_h - y;
  }
  if (pw <= 0 || h <= 0) {
    return;
  }

  this->wake_panel_();
  this->wait_for_display_ready_();

  this->lcd_write_cmd_code_(USDEF_I80_CMD_TEMP);
  this->lcd_write_data_(0x0001);  // Force Set from host
  this->lcd_write_data_(static_cast<uint16_t>(this->temperature_));

  this->it8951_write_reg_(UP1SR + 2, this->it8951_read_reg_(UP1SR + 2) & ~(1 << 2));
  this->set_img_buf_base_addr_(this->img_buf_addr_);
  this->it8951_load_img_area_start_(IT8951_LDIMG_L_ENDIAN, IT8951_4BPP, 0, static_cast<uint16_t>(px),
                                    static_cast<uint16_t>(y), static_cast<uint16_t>(pw),
                                    static_cast<uint16_t>(h));
  this->lcd_write_framebuffer_4bpp_area_(static_cast<uint16_t>(px), static_cast<uint16_t>(y),
                                         static_cast<uint16_t>(pw), static_cast<uint16_t>(h));
  this->lcd_write_cmd_code_(IT8951_TCON_LD_IMG_END);
  this->wait_for_display_ready_();

  ESP_LOGD(TAG, "Zone refresh panel x=%d y=%d w=%d h=%d mode=%d", px, y, pw, h, mode);
  this->it8951_display_area_(static_cast<uint16_t>(px), static_cast<uint16_t>(y), static_cast<uint16_t>(pw),
                             static_cast<uint16_t>(h), static_cast<uint16_t>(mode));
  this->wait_for_display_ready_();

  this->sleep_panel_();

  if (++this->partials_since_full_ >= MAX_PARTIALS_BEFORE_FULL) {
    ESP_LOGD(TAG, "Partial threshold reached, forcing full refresh to purge ghosting");
    this->full_refresh();
  }
}

void IT8951ReTerminalE1003Display::dump_config() {
  LOG_DISPLAY("", "IT8951 reTerminal E1003", this);
  ESP_LOGCONFIG(TAG, "  SPI Write Frequency: %u Hz", this->spi_frequency_);
  ESP_LOGCONFIG(TAG, "  SPI Read Frequency: %u Hz", this->spi_read_frequency_);
  ESP_LOGCONFIG(TAG, "  VCOM: %u", this->vcom_);
  ESP_LOGCONFIG(TAG, "  Temperature: %d°C", this->temperature_);
  ESP_LOGCONFIG(TAG, "  VCOM selector: 0x%04X", this->vcom_write_selector_);
  ESP_LOGCONFIG(TAG, "  Probe path: %s", this->probe_path_ != nullptr ? this->probe_path_ : "none");
  ESP_LOGCONFIG(TAG, "  DevInfo Panel: %ux%u", this->dev_info_.panel_width, this->dev_info_.panel_height);
  ESP_LOGCONFIG(TAG, "  DevInfo ImgBuf: 0x%04X%04X", this->dev_info_.img_buf_addr_h, this->dev_info_.img_buf_addr_l);
  ESP_LOGCONFIG(TAG, "  Buffer allocated: %s", this->framebuffer_ != nullptr ? "YES" : "NO");
  ESP_LOGCONFIG(TAG, "  Busy pin: %s", digitalRead(IT8951_PIN_BUSY) ? "HIGH (ready)" : "LOW (busy)");
  ESP_LOGCONFIG(TAG, "  VCOM read-back: %u (0x%04X)", this->probe_vcom_, this->probe_vcom_);
  if (this->fail_reason_ != nullptr) {
    ESP_LOGE(TAG, "  FAILURE REASON: %s", this->fail_reason_);
  }
  LOG_UPDATE_INTERVAL(this);
}

void IT8951ReTerminalE1003Display::fill(Color color) {
  if (this->framebuffer_ == nullptr) {
    return;
  }
  const uint8_t gray8 = static_cast<uint8_t>((77u * color.r + 150u * color.g + 29u * color.b) >> 8);
  const uint8_t nib = gray8 >> 4;
  const uint8_t byte = static_cast<uint8_t>((nib << 4) | nib);
  memset(this->framebuffer_, byte, static_cast<size_t>(this->get_width_internal()) * this->get_height_internal() / 2);
}

void IT8951ReTerminalE1003Display::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x < 0 || x >= this->get_width() || y < 0 || y >= this->get_height()) {
    return;
  }
  if (this->framebuffer_ == nullptr) {
    return;
  }

  x = this->get_width() - 1 - x;
  const uint32_t pos = (x + y * this->get_width()) / 2;
  const uint8_t gray8 = static_cast<uint8_t>((77u * color.r + 150u * color.g + 29u * color.b) >> 8);
  const uint8_t pixel_val = gray8 >> 4;  // 0x00=black, 0x0F=white

  if ((x % 2) == 0) {
    this->framebuffer_[pos] = (this->framebuffer_[pos] & 0xF0) | pixel_val;
  } else {
    this->framebuffer_[pos] = (this->framebuffer_[pos] & 0x0F) | (pixel_val << 4);
  }
}

uint8_t IT8951ReTerminalE1003Display::get_pixel_nibble_(uint16_t x, uint16_t y) {
  const uint32_t pos = (x + y * this->get_width()) / 2;
  if ((x & 1U) == 0) {
    return this->framebuffer_[pos] & 0x0F;
  }
  return this->framebuffer_[pos] >> 4;
}

// Lecture en coordonnees LOGIQUES: applique le meme mirror X que
// draw_absolute_pixel_internal (qui stocke l'image inversee en X).
uint8_t IT8951ReTerminalE1003Display::get_pixel_logical_(int x, int y) {
  return this->get_pixel_nibble_(static_cast<uint16_t>(this->get_width() - 1 - x), static_cast<uint16_t>(y));
}

// Ecriture d'un nibble (0=noir .. 0x0F=blanc) en coordonnees LOGIQUES, meme
// mapping que draw_absolute_pixel_internal mais sans conversion de couleur.
void IT8951ReTerminalE1003Display::set_pixel_nibble_(int x, int y, uint8_t nibble) {
  if (x < 0 || x >= this->get_width() || y < 0 || y >= this->get_height() || this->framebuffer_ == nullptr) {
    return;
  }
  x = this->get_width() - 1 - x;
  const uint32_t pos = (x + y * this->get_width()) / 2;
  nibble &= 0x0F;
  if ((x % 2) == 0) {
    this->framebuffer_[pos] = (this->framebuffer_[pos] & 0xF0) | nibble;
  } else {
    this->framebuffer_[pos] = (this->framebuffer_[pos] & 0x0F) | (nibble << 4);
  }
}

void IT8951ReTerminalE1003Display::lcd_send_cmd_arg_(uint16_t cmd, uint16_t *args, uint16_t num_args) {
  this->lcd_write_cmd_code_(cmd);
  for (uint16_t i = 0; i < num_args; i++) {
    this->lcd_write_data_(args[i]);
  }
}

uint16_t IT8951ReTerminalE1003Display::it8951_read_reg_(uint16_t addr) {
  this->lcd_write_cmd_code_(IT8951_TCON_REG_RD);
  this->lcd_write_data_(addr);
  return this->lcd_read_data_();
}

void IT8951ReTerminalE1003Display::it8951_write_reg_(uint16_t addr, uint16_t val) {
  this->lcd_write_cmd_code_(IT8951_TCON_REG_WR);
  this->lcd_write_data_(addr);
  this->lcd_write_data_(val);
}

void IT8951ReTerminalE1003Display::get_it8951_system_info_() {
  memset(&this->dev_info_, 0, sizeof(this->dev_info_));
  this->lcd_write_cmd_code_(USDEF_I80_CMD_GET_DEV_INFO);
  this->lcd_read_n_data_(reinterpret_cast<uint16_t *>(&this->dev_info_), sizeof(IT8951DevInfo) / 2);
}

void IT8951ReTerminalE1003Display::set_img_buf_base_addr_(uint32_t addr) {
  const uint16_t hi = (addr >> 16) & 0xFFFF;
  const uint16_t lo = addr & 0xFFFF;
  this->it8951_write_reg_(LISAR + 2, hi);
  this->it8951_write_reg_(LISAR, lo);
}

void IT8951ReTerminalE1003Display::it8951_load_img_area_start_(uint16_t endian, uint16_t pix_fmt, uint16_t rotate,
                                                                uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  uint16_t args[5];
  args[0] = (endian << 8) | (pix_fmt << 4) | rotate;
  args[1] = x;
  args[2] = y;
  args[3] = w;
  args[4] = h;
  this->lcd_send_cmd_arg_(IT8951_TCON_LD_IMG_AREA, args, 5);
}

void IT8951ReTerminalE1003Display::it8951_display_area_(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                                         uint16_t mode) {
  this->lcd_write_cmd_code_(USDEF_I80_CMD_DPY_AREA);
  this->lcd_write_data_(x);
  this->lcd_write_data_(y);
  this->lcd_write_data_(w);
  this->lcd_write_data_(h);
  this->lcd_write_data_(mode);
}

void IT8951ReTerminalE1003Display::it8951_display_area_1bpp_(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                                              uint16_t mode, uint8_t bg_gray, uint8_t fg_gray) {
  this->it8951_write_reg_(UP1SR + 2, this->it8951_read_reg_(UP1SR + 2) | (1 << 2));
  this->it8951_write_reg_(BGVR, (uint16_t(bg_gray) << 8) | fg_gray);
  this->it8951_display_area_(x, y, w, h, mode);
  this->wait_for_display_ready_();
}

void IT8951ReTerminalE1003Display::show_sd_image(const char *path) {
  if (this->framebuffer_ == nullptr) { ESP_LOGW(TAG, "show_sd_image: framebuffer absent"); return; }
  if (!this->sd_ok_) { ESP_LOGW(TAG, "show_sd_image: SD non initialisee"); return; }
  FsFile f = this->sd_.open(path, O_RDONLY);
  if (!f) { ESP_LOGW(TAG, "show_sd_image: ouverture echouee %s", path); return; }
  const size_t need = (size_t) this->get_width_internal() * this->get_height_internal() / 2;
  size_t got = 0;
  while (got < need) {
    size_t chunk = (need - got > 4096) ? 4096 : (need - got);
    int rd = f.read(this->framebuffer_ + got, chunk);
    if (rd <= 0) break;
    got += rd;
    if ((got & 0x3FFF) == 0) App.feed_wdt();
  }
  f.close();
  ESP_LOGI(TAG, "show_sd_image %s: %u/%u octets lus", path, (unsigned) got, (unsigned) need);
  if (got < need) memset(this->framebuffer_ + got, 0xFF, need - got);

  // Pousse le framebuffer en GC16 (photo = 16 gris, chemin 4bpp).
  this->wake_panel_();
  const uint16_t w = this->get_width_internal();
  const uint16_t h = this->get_height_internal();
  const uint16_t width_in_words = (w + 3) / 4;
  this->wait_for_display_ready_();
  this->lcd_write_cmd_code_(USDEF_I80_CMD_TEMP);
  this->lcd_write_data_(0x0001);
  this->lcd_write_data_(static_cast<uint16_t>(this->temperature_));
  this->it8951_write_reg_(UP1SR + 2, this->it8951_read_reg_(UP1SR + 2) & ~(1 << 2));
  this->set_img_buf_base_addr_(this->img_buf_addr_);
  this->it8951_load_img_area_start_(IT8951_LDIMG_L_ENDIAN, IT8951_4BPP, 0, 0, 0, w, h);
  this->lcd_write_framebuffer_4bpp_(reinterpret_cast<uint16_t *>(this->framebuffer_), width_in_words, h);
  this->lcd_write_cmd_code_(IT8951_TCON_LD_IMG_END);
  this->it8951_display_area_(0, 0, w, h, 0);  // INIT (purge ghost)
  this->wait_for_display_ready_();
  this->it8951_display_area_(0, 0, w, h, 2);  // GC16 (16 gris)
  this->wait_for_display_ready_();
  this->sleep_panel_();
  this->partials_since_full_ = 0;
}

bool IT8951ReTerminalE1003Display::sd_file_exists(const char *path) {
  if (!this->sd_ok_) {
    return false;
  }
  return this->sd_.exists(path);
}

void IT8951ReTerminalE1003Display::make_thumbnail(const char *src_path, const char *dst_path) {
  if (this->framebuffer_ == nullptr) {
    ESP_LOGW(TAG, "make_thumbnail: framebuffer absent");
    return;
  }
  if (!this->sd_ok_) {
    ESP_LOGW(TAG, "make_thumbnail: SD non initialisee");
    return;
  }
  // 1) Charger la source plein ecran dans le framebuffer (meme boucle que show_sd_image).
  FsFile f = this->sd_.open(src_path, O_RDONLY);
  if (!f) {
    ESP_LOGW(TAG, "make_thumbnail: ouverture source echouee %s", src_path);
    return;
  }
  const size_t need = (size_t) this->get_width_internal() * this->get_height_internal() / 2;
  size_t got = 0;
  while (got < need) {
    size_t chunk = (need - got > 4096) ? 4096 : (need - got);
    int rd = f.read(this->framebuffer_ + got, chunk);
    if (rd <= 0)
      break;
    got += rd;
    if ((got & 0x3FFF) == 0)
      App.feed_wdt();
  }
  f.close();
  if (got < need) {
    memset(this->framebuffer_ + got, 0xFF, need - got);
  }

  // 2) Sous-echantillonner (nearest) en lisant des pixels LOGIQUES, empaqueter
  //    2 pixels/octet en row-major LOGIQUE -> meme convention que draw_sd_thumbnail.
  FsFile o = this->sd_.open(dst_path, O_WRONLY | O_CREAT | O_TRUNC);
  if (!o) {
    ESP_LOGW(TAG, "make_thumbnail: creation dst echouee %s", dst_path);
    return;
  }
  const int row_bytes = THUMB_W / 2;  // 234
  uint8_t row[THUMB_W / 2];
  for (int ty = 0; ty < THUMB_H; ty++) {
    const int sy = ty * THUMB_FACTOR;
    for (int i = 0; i < row_bytes; i++) {
      const int tx = i * 2;
      const uint8_t lo = this->get_pixel_logical_(tx * THUMB_FACTOR, sy);
      const uint8_t hi = this->get_pixel_logical_((tx + 1) * THUMB_FACTOR, sy);
      row[i] = (lo & 0x0F) | (hi << 4);
    }
    o.write(row, row_bytes);
    if ((ty & 0x1F) == 0)
      App.feed_wdt();
  }
  o.close();
  ESP_LOGI(TAG, "make_thumbnail %s -> %s (%dx%d)", src_path, dst_path, THUMB_W, THUMB_H);
}

void IT8951ReTerminalE1003Display::draw_sd_thumbnail(const char *path, int dx, int dy) {
  if (this->framebuffer_ == nullptr || !this->sd_ok_) {
    return;
  }
  FsFile f = this->sd_.open(path, O_RDONLY);
  if (!f) {
    ESP_LOGW(TAG, "draw_sd_thumbnail: ouverture echouee %s", path);
    return;
  }
  const int row_bytes = THUMB_W / 2;  // 234
  uint8_t row[THUMB_W / 2];
  for (int ty = 0; ty < THUMB_H; ty++) {
    int rd = f.read(row, row_bytes);
    if (rd < row_bytes)
      break;
    for (int i = 0; i < row_bytes; i++) {
      const int tx = i * 2;
      this->set_pixel_nibble_(dx + tx, dy + ty, row[i] & 0x0F);
      this->set_pixel_nibble_(dx + tx + 1, dy + ty, row[i] >> 4);
    }
    if ((ty & 0x1F) == 0)
      App.feed_wdt();
  }
  f.close();
}

}  // namespace it8951_reterminal_e1003
}  // namespace esphome
