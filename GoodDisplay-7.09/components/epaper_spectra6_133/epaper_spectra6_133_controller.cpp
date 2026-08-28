#include "epaper_spectra6_133_controller.h"

/**
 * @file epaper_spectra6_133_controller.cpp
 * @brief Implements panel initialisation, refresh, and transfer sequencing.
 */

#include <algorithm>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esphome/core/application.h"

namespace esphome {
namespace epaper_spectra6_133 {

static const char *const CONTROLLER_TAG = "epaper_spectra6_133.controller";

namespace {

/**
 * @brief Converts a logical rectangle into a controller-aligned partial region
 *        for one driver IC half.
 *
 * Clips the rectangle to the panel, intersects with the IC's 600-column half,
 * aligns horizontally to 4-pixel boundaries (minimum 16 px width), and ensures
 * an even total row count.
 *
 * @param cs_index  0 = left 600 columns (IC0), 1 = right 600 columns (IC1).
 * @param x, y, width, height  Logical panel rectangle.
 * @param[out] region  Filled on success.
 * @return true if the rectangle overlaps this IC half after alignment.
 */
bool build_partial_region(uint8_t cs_index, int x, int y, int width, int height, PartialRegion *region) {
  if (region == nullptr || width <= 0 || height <= 0 || cs_index > 1) {
    return false;
  }

  const int clipped_x0 = std::max(0, x);
  const int clipped_y0 = std::max(0, y);
  const int clipped_x1 = std::min(EPD_WIDTH, x + width);
  const int clipped_y1 = std::min(EPD_HEIGHT, y + height);
  if (clipped_x0 >= clipped_x1 || clipped_y0 >= clipped_y1) {
    return false;
  }

  const int half_start = cs_index == 0 ? 0 : EPD_WIDTH / 2;
  const int half_end = cs_index == 0 ? EPD_WIDTH / 2 : EPD_WIDTH;
  const int overlap_x0 = std::max(clipped_x0, half_start);
  const int overlap_x1 = std::min(clipped_x1, half_end);
  if (overlap_x0 >= overlap_x1) {
    return false;
  }

  // Align horizontal bounds to 4-pixel boundaries within the IC half.
  int aligned_x0 = (overlap_x0 - half_start) & ~0x03;
  int aligned_x1 = ((overlap_x1 - half_start) + 3) & ~0x03;

  // Enforce the minimum 16-pixel width required by the controller.
  if ((aligned_x1 - aligned_x0) < 16) {
    aligned_x1 = aligned_x0 + 16;
  }
  if (aligned_x1 > EPD_WIDTH / 2) {
    aligned_x0 -= aligned_x1 - (EPD_WIDTH / 2);
    aligned_x1 = EPD_WIDTH / 2;
  }
  if (aligned_x0 < 0) {
    aligned_x1 = std::min(EPD_WIDTH / 2, aligned_x1 - aligned_x0);
    aligned_x0 = 0;
  }
  if (aligned_x1 <= aligned_x0) {
    return false;
  }

  // Adjust vertical bounds so that y_start is even and the total height
  // is even.  The controller addresses rows in 2-row units (VRST = y/2),
  // so an odd y_start would misalign the transferred pixel data.
  int adjusted_y0 = clipped_y0 & ~0x01;  // round down to even
  int adjusted_y1 = clipped_y1;
  if ((adjusted_y1 - adjusted_y0) & 0x01) {
    if (adjusted_y1 < EPD_HEIGHT) {
      adjusted_y1++;
    } else if (adjusted_y0 > 0) {
      adjusted_y0 -= 2;  // already even, step back by 2 to stay even
    }
  }
  if (adjusted_y0 < 0) {
    adjusted_y0 = 0;
  }
  if (adjusted_y0 >= adjusted_y1) {
    return false;
  }

  region->cs_index = cs_index;
  region->x_start = static_cast<uint16_t>(aligned_x0);
  region->y_start = static_cast<uint16_t>(adjusted_y0);
  region->width = static_cast<uint16_t>(aligned_x1 - aligned_x0);
  region->height = static_cast<uint16_t>(adjusted_y1 - adjusted_y0);
  region->row_byte_offset = (cs_index == 0 ? 0 : HALF_ROW_BYTES) + static_cast<size_t>(aligned_x0 / 2);
  region->row_byte_count = static_cast<size_t>(region->width / 2);
  return true;
}

}  // anonymous namespace

/**
 * @brief Executes the vendor register initialisation sequence after reset.
 *
 * Most configuration commands are broadcast to both controller halves,
 * while power-stage commands are written to CS0 only.
 */
bool Controller::init_panel() {
  // Static register payloads used during panel startup.
  const uint8_t an_tm[] = {0xC0, 0x1C, 0x1C, 0xCC, 0xCC, 0xCC, 0x15, 0x15, 0x55};
  const uint8_t cmd66[] = {0x49, 0x55, 0x13, 0x5D, 0x05, 0x10};
  const uint8_t psr[] = {0xDF, 0x69};
  const uint8_t cdi[] = {0xF7};
  const uint8_t tcon[] = {0x03, 0x03};
  const uint8_t agid[] = {0x10};
  const uint8_t pws[] = {0x22};
  const uint8_t ccset[] = {0x01};
  const uint8_t tres[] = {0x04, 0xB0, 0x03, 0x20};
  const uint8_t pwr[] = {0x0F, 0x00, 0x28, 0x2C, 0x28, 0x38};
  const uint8_t en_buf[] = {0x07};
  const uint8_t btst_p[] = {0xE8, 0x28};
  const uint8_t boost_vddp_en[] = {0x01};
  const uint8_t btst_n[] = {0xE8, 0x28};
  const uint8_t buck_boost_vddn[] = {0x01};
  const uint8_t tft_vcom_power[] = {0x02};

  this->transport_.hardware_reset();
  if (!this->transport_.wait_busy_high()) {
    ESP_LOGE(CONTROLLER_TAG, "BUSY not released after reset");
    return false;
  }

  auto epd_cs0 = [&](uint8_t cmd, const uint8_t *data, size_t len) -> bool {
    this->transport_.set_cs(0, 0);
    if (this->transport_.write_register(cmd, data, len) != ESP_OK) {
      this->transport_.set_cs_all(1);
      return false;
    }
    this->transport_.set_cs_all(1);
    return true;
  };
  auto epd_all = [&](uint8_t cmd, const uint8_t *data, size_t len) -> bool {
    this->transport_.set_cs_all(0);
    if (this->transport_.write_register(cmd, data, len) != ESP_OK) {
      this->transport_.set_cs_all(1);
      return false;
    }
    this->transport_.set_cs_all(1);
    return true;
  };

  if (!epd_cs0(AN_TM, an_tm, sizeof(an_tm)))
    return false;
  if (!epd_all(CMD66, cmd66, sizeof(cmd66)))
    return false;
  if (!epd_all(PSR, psr, sizeof(psr)))
    return false;
  if (!epd_all(CDI, cdi, sizeof(cdi)))
    return false;
  if (!epd_all(TCON, tcon, sizeof(tcon)))
    return false;
  if (!epd_all(AGID, agid, sizeof(agid)))
    return false;
  if (!epd_all(PWS, pws, sizeof(pws)))
    return false;
  if (!epd_all(CCSET, ccset, sizeof(ccset)))
    return false;
  if (!epd_all(TRES, tres, sizeof(tres)))
    return false;
  if (!epd_cs0(PWR, pwr, sizeof(pwr)))
    return false;
  if (!epd_cs0(EN_BUF, en_buf, sizeof(en_buf)))
    return false;
  if (!epd_cs0(BTST_P, btst_p, sizeof(btst_p)))
    return false;
  if (!epd_cs0(BOOST_VDDP_EN, boost_vddp_en, sizeof(boost_vddp_en)))
    return false;
  if (!epd_cs0(BTST_N, btst_n, sizeof(btst_n)))
    return false;
  if (!epd_cs0(BUCK_BOOST_VDDN, buck_boost_vddn, sizeof(buck_boost_vddn)))
    return false;
  if (!epd_cs0(TFT_VCOM_POWER, tft_vcom_power, sizeof(tft_vcom_power)))
    return false;

  this->initialized_ = true;
  return true;
}

/**
 * @brief Powers the panel on, triggers a refresh, then powers it off again.
 *
 * This follows the same PON -> DRF -> POF order used by the reference
 * implementation and waits for BUSY to return high after each step.
 * Delegates each command to the step-based send_refresh_*() primitives
 * so the cooperative operation path can reuse the same underlying operations.
 */
bool Controller::refresh() {
  if (!this->send_refresh_pon()) {
    return false;
  }
  if (!this->transport_.wait_busy_high()) {
    return false;
  }
  this->transport_.delay_ms(30);
  if (!this->send_refresh_drf()) {
    return false;
  }
  if (!this->transport_.wait_busy_high(20000)) {
    return false;
  }
  if (!this->send_refresh_pof()) {
    return false;
  }
  if (!this->transport_.wait_busy_high()) {
    return false;
  }
  return true;
}

/**
 * @brief Transfers the entire logical framebuffer to both driver ICs.
 *
 * Each controller half receives one contiguous 300-byte slice per row,
 * with watchdog feeds and short delays inserted between rows.
 * Uses the step-based begin/write/end primitives so the cooperative operation path
 * can reuse the same streaming logic.
 */
bool Controller::transfer_full_frame(const uint8_t *framebuffer) {
  if (framebuffer == nullptr) {
    ESP_LOGW(CONTROLLER_TAG, "Full-frame transfer skipped: framebuffer is null");
    return false;
  }

  for (uint8_t half = 0; half < 2; half++) {
    if (!this->begin_half_transfer(half)) {
      return false;
    }
    for (int row = 0; row < EPD_HEIGHT; row++) {
      if (!this->write_half_row(framebuffer, row, half)) {
        this->end_half_transfer();
        return false;
      }
      if ((row & 0x0F) == 0) {
        App.feed_wdt();
        vTaskDelay(pdMS_TO_TICKS(1));
      }
    }
    this->end_half_transfer();
  }

  if (!this->refresh()) {
    return false;
  }

  vTaskDelay(pdMS_TO_TICKS(10));
  return true;
}

/**
 * @brief Transfers only a logical sub-rectangle using controller partial regions.
 *
 * The rectangle is converted into zero, one, or two aligned controller
 * regions depending on which 600-column halves it overlaps.
 *
 * Transfer phase (per IC with real changes):
 *   CMD66 → PTLW → DTM (pixel rows)
 * Each IC receives its sequence atomically.
 *
 * Refresh phase (shared BUSY constraint):
 *   Both ICs share one BUSY line. After PON every IC must complete a
 *   DRF cycle before BUSY goes high. An IC that has no dirty region
 *   still needs a PTLW so that DRF refreshes only a minimal 16×2 px
 *   region from its existing buffer instead of the full screen.
 *   See the header comment on transfer_region() for background.
 */
bool Controller::transfer_region(const uint8_t *framebuffer, int x, int y, int width, int height) {
  if (framebuffer == nullptr) {
    ESP_LOGW(CONTROLLER_TAG, "Partial refresh skipped: framebuffer is null");
    return false;
  }
  if (width <= 0 || height <= 0) {
    ESP_LOGW(CONTROLLER_TAG, "Partial refresh skipped: invalid rectangle x=%d y=%d w=%d h=%d", x, y, width, height);
    return false;
  }

  // A logical rectangle may touch neither, one, or both 600-column
  // controller halves. Each half gets its own aligned PTLW region.
  PartialRegion regions[2]{};
  bool has_region[2] = {
      build_partial_region(0, x, y, width, height, &regions[0]),
      build_partial_region(1, x, y, width, height, &regions[1]),
  };

  if (!has_region[0] && !has_region[1]) {
    ESP_LOGW(CONTROLLER_TAG, "Partial refresh skipped: requested rectangle is outside the panel");
    return false;
  }

  this->transport_.set_cs_all(1);

  auto fail = [&]() {
    this->disable_partial_regions();
    this->transport_.set_cs_all(1);
    return false;
  };

  // Phase 1 — Transfer pixel data to each IC that has real changes.
  //
  // Each IC receives CMD66 → PTLW → DTM as one uninterrupted
  // sequence so that the partial region is active while pixel data
  // is clocked in. ICs without changes are skipped here and handled
  // in Phase 2 below.  Uses the step-based begin/write/end primitives
  // so the cooperative operation path can reuse the same streaming logic.
  for (size_t index = 0; index < 2; index++) {
    if (!has_region[index]) {
      continue;
    }
    const PartialRegion &region = regions[index];
    if (!this->begin_region_transfer(region)) {
      return fail();
    }
    for (uint16_t row = 0; row < region.height; row++) {
      if (!this->write_region_row(framebuffer, region, row)) {
        this->end_region_transfer(region.cs_index);
        return fail();
      }
      if ((row & 0x0F) == 0) {
        App.feed_wdt();
        vTaskDelay(pdMS_TO_TICKS(1));
      }
    }
    this->end_region_transfer(region.cs_index);
  }

  // Phase 2 — Arm a dummy partial region on every IC that had no
  //           real changes, so DRF doesn't trigger a full-screen
  //           refresh on that half.
  //
  // The two ICs share one BUSY line. refresh() (PON → DRF → POF)
  // must address both ICs; an IC without an active PTLW would
  // interpret DRF as a full-frame refresh — visually re-rendering
  // the entire unchanged half.
  //
  // Solution: program a minimal 16×2 px PTLW (the smallest region
  // that satisfies the controller's alignment rules) without sending
  // any DTM data.  The IC refreshes that tiny region from its
  // existing internal buffer, producing no visible change.
  //
  // This uses CMD66 → PTLW without DTM on unchanged halves.
  for (size_t index = 0; index < 2; index++) {
    if (has_region[index]) {
      continue;
    }
    if (!this->arm_dummy_region(static_cast<uint8_t>(index))) {
      return fail();
    }
  }

  // Phase 3 — Refresh and tear-down.
  //
  // refresh() broadcasts PON → DRF → POF to both ICs.  Each IC now
  // has an active PTLW (real or dummy), so DRF only refreshes the
  // partial region on each half.
  if (!this->refresh()) {
    return fail();
  }

  this->transport_.delay_ms(300);
  this->disable_partial_regions();  // clear PTLW state for next operation
  return true;
}

/**
 * @brief Reads the vendor status register from both controller halves.
 *
 * The first status byte is expected to report bit 0 set when the driver
 * IC is responsive.
 */
bool Controller::probe_drivers() {
  bool all_ready = true;
  uint8_t data[3] = {0, 0, 0};

  for (uint8_t cs_index = 0; cs_index < 2; cs_index++) {
    std::memset(data, 0, sizeof(data));
    this->transport_.set_cs(cs_index, 0);
    const esp_err_t err = this->transport_.read_register(0xF2, data, sizeof(data));
    this->transport_.set_cs(cs_index, 1);

    if (err != ESP_OK) {
      ESP_LOGE(CONTROLLER_TAG, "Driver IC %u read failed: %s", cs_index, esp_err_to_name(err));
      all_ready = false;
      continue;
    }

    ESP_LOGI(CONTROLLER_TAG, "Driver IC %u = 0x%02X 0x%02X 0x%02X", cs_index, data[0], data[1], data[2]);
    if ((data[0] & 0x01U) != 0x01U) {
      all_ready = false;
    }
  }

  return all_ready;
}

// =============================================================================
// Step-based primitives — used by both the blocking API above and the
// component's cooperative display operation loop() implementation.
// =============================================================================

/**
 * @brief Selects CS for @p half and sends the DTM command; CS stays LOW.
 *
 * The caller must stream all pixel rows with write_half_row() and then
 * call end_half_transfer() to release CS before any other operation.
 */
bool Controller::begin_half_transfer(uint8_t half) {
  if (half > 1) {
    ESP_LOGW(CONTROLLER_TAG, "Half transfer start skipped: invalid half index %u", static_cast<unsigned>(half));
    return false;
  }

  this->transport_.set_cs(half, 0);
  if (this->transport_.write_command(DTM) != ESP_OK) {
    this->transport_.set_cs_all(1);
    return false;
  }
  return true;
}

/**
 * @brief Sends one row of HALF_ROW_BYTES to the IC selected by @p half.
 *
 * CS must already be LOW (established by begin_half_transfer()).
 * Row @p row is the logical 0-based panel row (0 … EPD_HEIGHT-1).
 */
bool Controller::write_half_row(const uint8_t *framebuffer, int row, uint8_t half) {
  if (framebuffer == nullptr) {
    ESP_LOGW(CONTROLLER_TAG, "Half-row write skipped: framebuffer is null");
    return false;
  }
  if (row < 0 || row >= EPD_HEIGHT) {
    ESP_LOGW(CONTROLLER_TAG, "Half-row write skipped: row %d is outside 0...%d", row, EPD_HEIGHT - 1);
    return false;
  }
  if (half > 1) {
    ESP_LOGW(CONTROLLER_TAG, "Half-row write skipped: invalid half index %u", static_cast<unsigned>(half));
    return false;
  }

  const size_t offset = static_cast<size_t>(row) * ROW_BYTES + (half == 0 ? 0 : HALF_ROW_BYTES);
  return this->transport_.write_data(framebuffer + offset, HALF_ROW_BYTES) == ESP_OK;
}

/** @brief Releases all CS pins after a half-transfer streaming phase. */
void Controller::end_half_transfer() { this->transport_.set_cs_all(1); }

/**
 * @brief Sends CMD66 + PTLW + DTM for @p region; CS for region.cs_index stays LOW.
 *
 * The caller must stream all pixel rows with write_region_row() and then
 * call end_region_transfer() to release CS before any other operation.
 */
bool Controller::begin_region_transfer(const PartialRegion &region) {
  if (region.cs_index > 1) {
    ESP_LOGW(CONTROLLER_TAG, "Region transfer start skipped: invalid cs_index %u",
             static_cast<unsigned>(region.cs_index));
    return false;
  }
  if (region.width == 0 || region.height == 0) {
    ESP_LOGW(CONTROLLER_TAG, "Region transfer start skipped: invalid region dimensions w=%u h=%u",
             static_cast<unsigned>(region.width), static_cast<unsigned>(region.height));
    return false;
  }

  if (!this->enable_partial_region(region)) {
    return false;
  }
  // enable_partial_region() leaves CS HIGH.  Pull the IC's CS LOW for DTM payload.
  this->transport_.set_cs(region.cs_index, 0);
  if (this->transport_.write_command(DTM) != ESP_OK) {
    this->transport_.set_cs_all(1);
    return false;
  }
  return true;
}

/**
 * @brief Sends one region pixel row for the IC described by @p region.
 *
 * CS must already be LOW (established by begin_region_transfer()).
 * @p row is 0-based within the region's height (0 … region.height-1).
 */
bool Controller::write_region_row(const uint8_t *framebuffer, const PartialRegion &region, int row) {
  if (framebuffer == nullptr) {
    ESP_LOGW(CONTROLLER_TAG, "Region-row write skipped: framebuffer is null");
    return false;
  }
  if (region.cs_index > 1) {
    ESP_LOGW(CONTROLLER_TAG, "Region-row write skipped: invalid cs_index %u", static_cast<unsigned>(region.cs_index));
    return false;
  }
  if (row < 0 || row >= region.height) {
    ESP_LOGW(CONTROLLER_TAG, "Region-row write skipped: row %d is outside 0...%u", row,
             static_cast<unsigned>(region.height - 1));
    return false;
  }

  const size_t row_index = static_cast<size_t>(region.y_start + static_cast<uint16_t>(row)) * ROW_BYTES;
  const uint8_t *row_ptr = framebuffer + row_index + region.row_byte_offset;
  return this->transport_.write_data(row_ptr, region.row_byte_count) == ESP_OK;
}

/** @brief Releases all CS pins after a region streaming phase. */
void Controller::end_region_transfer(uint8_t /*cs_index*/) { this->transport_.set_cs_all(1); }

/**
 * @brief Programs a minimal 16×2 dummy PTLW on @p cs_index.
 *
 * No DTM data is sent.  This prevents a subsequent DRF from triggering
 * a full-screen refresh on the IC — it will only re-render the tiny
 * dummy window from its existing internal buffer.
 * Fully atomic: CS cycles internally via enable_partial_region().
 */
bool Controller::arm_dummy_region(uint8_t cs_index) {
  if (cs_index > 1) {
    ESP_LOGW(CONTROLLER_TAG, "Dummy region skipped: invalid cs_index %u", static_cast<unsigned>(cs_index));
    return false;
  }

  PartialRegion dummy{};
  dummy.cs_index = cs_index;
  dummy.x_start = 0;
  dummy.y_start = 0;
  dummy.width = 16;           // minimum: HRED - HRST + 1 >= 32 px after ×2 encoding
  dummy.height = 2;           // minimum: one two-row unit (VRST/VRED are row÷2)
  dummy.row_byte_offset = 0;  // unused — no DTM data follows
  dummy.row_byte_count = 0;   // unused — no DTM data follows
  return this->enable_partial_region(dummy);
}

/**
 * @brief Fills @p out[2] and @p has_region[2] with aligned PartialRegion descriptors.
 *
 * Exposes the anonymous-namespace build_partial_region() so that callers
 * outside this translation unit (e.g. the operation INIT stage handler) can
 * compute the per-IC regions without duplicating alignment logic.
 */
void Controller::compute_partial_regions(int x, int y, int width, int height, PartialRegion out[2],
                                         bool has_region[2]) const {
  has_region[0] = build_partial_region(0, x, y, width, height, &out[0]);
  has_region[1] = build_partial_region(1, x, y, width, height, &out[1]);
}

/**
 * @brief Sends the PON command (CS all → low, write PON, CS all → high).
 *
 * The caller is responsible for polling is_display_busy() until the panel
 * reports idle before proceeding to send_refresh_drf().
 */
bool Controller::send_refresh_pon() {
  this->transport_.set_cs_all(0);
  if (this->transport_.write_command(PON) != ESP_OK) {
    this->transport_.set_cs_all(1);
    return false;
  }
  this->transport_.set_cs_all(1);
  return true;
}

/**
 * @brief Sends the DRF register write (CS all → low, write DRF, CS all → high).
 *
 * The caller must have already waited for the 30 ms post-PON delay and
 * must poll is_display_busy() until idle after this call (up to ~20 s).
 */
bool Controller::send_refresh_drf() {
  this->transport_.set_cs_all(0);
  const uint8_t drf_data[] = {0x01};
  if (this->transport_.write_register(DRF, drf_data, sizeof(drf_data)) != ESP_OK) {
    this->transport_.set_cs_all(1);
    return false;
  }
  this->transport_.set_cs_all(1);
  return true;
}

/**
 * @brief Sends the POF register write (CS all → low, write POF, CS all → high).
 *
 * The caller must poll is_display_busy() until idle after this call.
 */
bool Controller::send_refresh_pof() {
  this->transport_.set_cs_all(0);
  const uint8_t pof_data[] = {0x00};
  if (this->transport_.write_register(POF, pof_data, sizeof(pof_data)) != ESP_OK) {
    this->transport_.set_cs_all(1);
    return false;
  }
  this->transport_.set_cs_all(1);
  return true;
}

/**
 * @brief Sends the panel deep-sleep command (DSLP, data 0xA5).
 *
 * The GDEP133C02 datasheet specifies command 0x07 with data byte 0xA5 to
 * enter deep sleep.  The command is only safe after BUSY has returned high.
 */
bool Controller::send_deep_sleep() {
  this->transport_.set_cs_all(0);
  const uint8_t sleep_data[] = {0xA5};
  if (this->transport_.write_register(DSLP, sleep_data, sizeof(sleep_data)) != ESP_OK) {
    this->transport_.set_cs_all(1);
    return false;
  }
  this->transport_.set_cs_all(1);
  return true;
}

/** @brief Returns true while the panel is busy (BUSY pin LOW = working). */
bool Controller::is_display_busy() const { return this->transport_.busy_level() == 0; }

/** @brief Disables any previously programmed partial-region mode on both halves. */
void Controller::disable_partial_regions() {
  const uint8_t partial_region_data[9] = {0, 0, 0, 0, 0, 0, 0, 0, PTLW_DISABLE};
  this->transport_.set_cs_all(0);
  (void) this->transport_.write_register(PTLW, partial_region_data, sizeof(partial_region_data));
  this->transport_.set_cs_all(1);
}

/**
 * @brief Programs one already aligned partial region into one controller half.
 *
 * Horizontal bounds are encoded in half-local pixel coordinates, while
 * vertical bounds are encoded in two-row units as required by the panel.
 */
bool Controller::enable_partial_region(const PartialRegion &region) {
  if (region.cs_index > 1) {
    return false;
  }

  const uint8_t cmd66[] = {0x49, 0x55, 0x13, 0x5D, 0x05, 0x10};
  // The controller's horizontal region is expressed in pixels inside
  // one 600-column half, while the vertical coordinates are addressed
  // in two-row units. build_partial_region() already enforces the
  // required width and parity constraints before we pack them here.
  const uint16_t horizontal_start = static_cast<uint16_t>(region.x_start * 2U);
  const uint16_t horizontal_end = static_cast<uint16_t>((region.x_start + region.width) * 2U - 1U);
  const uint16_t vertical_start = static_cast<uint16_t>(region.y_start / 2U);
  const uint16_t vertical_end = static_cast<uint16_t>((region.y_start + region.height) / 2U - 1U);
  const uint8_t partial_region_data[9] = {
      static_cast<uint8_t>(horizontal_start >> 8),
      static_cast<uint8_t>(horizontal_start & 0xFF),
      static_cast<uint8_t>(horizontal_end >> 8),
      static_cast<uint8_t>(horizontal_end & 0xFF),
      static_cast<uint8_t>(vertical_start >> 8),
      static_cast<uint8_t>(vertical_start & 0xFF),
      static_cast<uint8_t>(vertical_end >> 8),
      static_cast<uint8_t>(vertical_end & 0xFF),
      PTLW_ENABLE,
  };

  this->transport_.set_cs(region.cs_index, 0);
  if (this->transport_.write_register(CMD66, cmd66, sizeof(cmd66)) != ESP_OK) {
    this->transport_.set_cs_all(1);
    return false;
  }
  this->transport_.set_cs_all(1);

  this->transport_.set_cs(region.cs_index, 0);
  if (this->transport_.write_register(PTLW, partial_region_data, sizeof(partial_region_data)) != ESP_OK) {
    this->transport_.set_cs_all(1);
    return false;
  }
  this->transport_.set_cs_all(1);
  return true;
}

}  // namespace epaper_spectra6_133
}  // namespace esphome
