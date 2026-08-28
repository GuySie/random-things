#include "esphome/core/application.h"

/**
 * @file epaper_spectra6_133.cpp
 * @brief Implements the ESPHome-facing display component lifecycle and drawing hooks.
 */

#include "epaper_spectra6_133.h"
#include "epaper_spectra6_133_framebuffer.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esphome/core/log.h"

namespace esphome {
namespace epaper_spectra6_133 {

static const char *const TAG = "epaper_spectra6_133";

// Number of pixel rows streamed to the panel per loop() call during cooperative
// display operation execution. Each row is HALF_ROW_BYTES (300 bytes); at 10 MHz SPI this
// equates to roughly 1.9 ms of bus time per call, keeping the ESPHome main
// loop responsive throughout a full-frame transfer (~200 loop steps total).
static constexpr int OPERATION_ROWS_PER_STEP = 8;

// Timeout for the DRF busy-wait in a refresh operation (microseconds).
//
// PATCHED locally (see ../../reference/esphome-t133a01-driver/NOTES.md):
// upstream's 20s value is too tight for the Good Display GDEB0709E01
// (7.09" panel, same GDEP133C02 controller/resolution as the 13.3" panel
// this component targets). Real hardware exceeded 20s and hit this
// timeout even though the panel completed the refresh correctly — the
// controller IC keeps running the refresh from its own clock regardless
// of whether the host is still watching BUSY, so a too-short host-side
// timeout doesn't corrupt the image, but it does skip the POF/sleep
// commands (abort_display_operation_() never sends POF), leaving the
// panel powered without a clean shutdown. Bumped to 60s for safety
// margin until we have precise real-world timing data.
static constexpr int64_t REFRESH_BUSY_TIMEOUT_US = 60000LL * 1000LL;  // 60 s (patched, was 20 s)

// Post-PON delay before DRF (microseconds).
static constexpr int64_t PRE_REFRESH_DELAY_US = 30LL * 1000LL;  // 30 ms

// Post-refresh delay after a full-frame transfer (microseconds).
static constexpr int64_t POST_FULL_REFRESH_DELAY_US = 10LL * 1000LL;  // 10 ms

// Post-refresh delay after a region transfer (microseconds).
static constexpr int64_t POST_REGION_REFRESH_DELAY_US = 300LL * 1000LL;  // 300 ms

// Full-frame online images draw 1.92M pixels synchronously through ESPHome's
// DisplayBuffer path. Insert real scheduler delays so the ESP-IDF idle task
// can run during long draws and the task watchdog does not reset the device.
static constexpr uint32_t DRAW_PIXELS_PER_DELAY = 4096;

namespace {

struct HorizontalInterval {
  int x0;
  int x1;
};

bool partial_regions_contain_tracked_region(const PartialRegion regions[2], const bool has_region[2],
                                            const UpdateRegion &tracked) {
  if (tracked.empty()) {
    return true;
  }

  const int target_x0 = tracked.x;
  const int target_y0 = tracked.y;
  const int target_x1 = tracked.x + tracked.width;
  const int target_y1 = tracked.y + tracked.height;
  HorizontalInterval intervals[2];
  int interval_count = 0;

  for (int i = 0; i < 2; i++) {
    if (!has_region[i]) {
      continue;
    }
    const PartialRegion &region = regions[i];
    const int region_y0 = region.y_start;
    const int region_y1 = static_cast<int>(region.y_start) + static_cast<int>(region.height);
    if (region_y0 > target_y0 || region_y1 < target_y1) {
      continue;
    }

    const int half_start = region.cs_index == 0 ? 0 : EPD_WIDTH / 2;
    intervals[interval_count++] = {
        half_start + static_cast<int>(region.x_start),
        half_start + static_cast<int>(region.x_start) + static_cast<int>(region.width),
    };
  }

  if (interval_count == 2 && intervals[1].x0 < intervals[0].x0) {
    const HorizontalInterval tmp = intervals[0];
    intervals[0] = intervals[1];
    intervals[1] = tmp;
  }

  int covered_until = target_x0;
  for (int i = 0; i < interval_count; i++) {
    if (intervals[i].x1 <= covered_until) {
      continue;
    }
    if (intervals[i].x0 > covered_until) {
      return false;
    }
    covered_until = intervals[i].x1;
    if (covered_until >= target_x1) {
      return true;
    }
  }
  return false;
}

}  // namespace

EpaperSpectra6133::EpaperSpectra6133() : controller_(this->transport_) {}

/**
 * @brief Allocates the framebuffer and initializes GPIO/SPI/EPD hardware.
 *
 * Called once by ESPHome during the component startup phase.
 */
void EpaperSpectra6133::setup() {
  ESP_LOGI(TAG, "Setup started");
  this->init_internal_(FULL_FRAME_SIZE);
  if (this->buffer_ == nullptr) {
    ESP_LOGE(TAG, "Framebuffer allocation failed (%u bytes)", static_cast<unsigned>(FULL_FRAME_SIZE));
    this->mark_failed(LOG_STR("Display framebuffer unavailable"));
    return;
  }

  // On ESP32-S3 the PSRAM address space starts at 0x3C000000. Log a warning
  // if the 960 KB framebuffer somehow landed in internal SRAM (shouldn't
  // happen with ExternalRAMAllocator, but guards against future regressions).
  const bool in_psram = (reinterpret_cast<uintptr_t>(this->buffer_) >= 0x3C000000UL);
  if (!in_psram) {
    ESP_LOGW(TAG, "Framebuffer NOT in PSRAM (addr=0x%08X) — performance may suffer",
             static_cast<unsigned>(reinterpret_cast<uintptr_t>(this->buffer_)));
  }
  ESP_LOGI(TAG, "Framebuffer ready (%u bytes%s)", static_cast<unsigned>(FULL_FRAME_SIZE), in_psram ? ", PSRAM" : "");

  if (this->refresh_mode_ == RefreshMode::PARTIAL && this->change_detection_mode_ == ChangeDetectionMode::COMPARE) {
    // The previous-frame buffer is allocated on the first successful refresh
    // so the panel state is never assumed before a known-good transfer.
    ESP_LOGI(TAG, "Refresh mode: partial (compare) — previous-frame buffer deferred to first refresh");
  } else if (this->refresh_mode_ == RefreshMode::PARTIAL) {
    ESP_LOGI(TAG, "Refresh mode: partial (track)");
  }

  if (!this->initialize()) {
    ESP_LOGE(TAG, "Initialization failed");
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "Setup complete");
}

/**
 * @brief Prints the full component configuration to the ESPHome log.
 *
 * Outputs pin assignments, SPI host, display resolution, buffer size, and
 * current initialization status at CONFIG log level.
 */
void EpaperSpectra6133::dump_config() {
  ESP_LOGCONFIG(TAG, "epaper_spectra6_133:");
  ESP_LOGCONFIG(TAG, "  Version: %s", VERSION);
  ESP_LOGCONFIG(TAG, "  Resolution: %dx%d", EPD_WIDTH, EPD_HEIGHT);
  ESP_LOGCONFIG(TAG, "  SPI host: %d (SPI3_HOST=2)", static_cast<int>(this->transport_.spi_host()));
  ESP_LOGCONFIG(TAG, "  CS0 pin: %d", this->transport_.cs0_pin());
  ESP_LOGCONFIG(TAG, "  CS1 pin: %d", this->transport_.cs1_pin());
  ESP_LOGCONFIG(TAG, "  CLK pin: %d", this->transport_.clk_pin());
  ESP_LOGCONFIG(TAG, "  DATA0 pin: %d", this->transport_.data0_pin());
  ESP_LOGCONFIG(TAG, "  DATA1 pin: %d", this->transport_.data1_pin());
  ESP_LOGCONFIG(TAG, "  BUSY pin: %d", this->transport_.busy_pin());
  ESP_LOGCONFIG(TAG, "  RESET pin: %d", this->transport_.reset_pin());
  ESP_LOGCONFIG(TAG, "  POWER pin: %d", this->transport_.power_pin());
  ESP_LOGCONFIG(TAG, "  Buffer: %u bytes", static_cast<unsigned>(FULL_FRAME_SIZE));
  ESP_LOGCONFIG(TAG, "  Change detection mode: %s",
                this->change_detection_mode_ == ChangeDetectionMode::COMPARE ? "compare" : "track");
  ESP_LOGCONFIG(TAG, "  Refresh mode: %s", this->refresh_mode_ == RefreshMode::PARTIAL ? "partial" : "full");
  ESP_LOGCONFIG(TAG, "  Clear color: rgb(%u, %u, %u)", this->clear_color_.red, this->clear_color_.green,
                this->clear_color_.blue);
  ESP_LOGCONFIG(TAG, "  Auto sleep: %s", this->auto_sleep_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Power off after sleep: %s", this->power_off_after_sleep_ ? "yes" : "no");
  ESP_LOGCONFIG(
      TAG, "  Status: %s",
      this->is_failed()
          ? "failed"
          : (this->sleeping_ ? "sleeping" : (this->controller_.is_initialized() ? "ready" : "not initialized")));
  LOG_UPDATE_INTERVAL(this);
}

/** @brief Returns HARDWARE priority so this component is set up before application-level components. */
float EpaperSpectra6133::get_setup_priority() const { return setup_priority::HARDWARE; }

void EpaperSpectra6133::set_update_mode(RefreshMode mode) {
  ESP_LOGW(TAG, "set_update_mode() is deprecated and will be removed in version 1.0; use set_refresh_mode()");
  this->set_refresh_mode(mode);
}

/**
 * @brief Schedules a full-frame display update and returns immediately.
 *
 * Called automatically by ESPHome on the configured update_interval.
 * The display lambda is re-run and the resulting framebuffer is transferred
 * to the panel cooperatively across successive loop() calls.  When
 * refresh_mode is PARTIAL, only the detected changed region is refreshed.
 *
 * If another display operation is already in progress it is superseded:
 * the previous operation is cancelled and this one takes its place.
 */
void EpaperSpectra6133::update() {
  ESP_LOGD(TAG, "Update requested");
  this->request_display_operation_(DisplayOperationType::UPDATE, 0, 0, 0, 0, "update");
}

/**
 * @brief Fills the entire framebuffer with a single ESPHome color.
 *
 * Converts @p color to the closest 4-bit EPD nibble value and writes the
 * packed byte across every byte of the framebuffer.  Does not trigger a
 * panel refresh; the next update() or transfer_full_frame() call will
 * push the result to the display.
 */
void EpaperSpectra6133::fill(Color color) { fill_buffer_with_code(this->buffer_, color_to_code(color)); }

/**
 * @brief Performs the full hardware initialization sequence: GPIO → SPI → EPD.
 *
 * Configures output/input GPIO pins, initializes the SPI master bus and
 * device handle, activates LOAD_SW, and sends the complete EPD command
 * sequence (hardware reset + ~16 register writes).  Sets initialized_ = true
 * on success.  Returns false and leaves the component in a recoverable state
 * on any hardware error.
 */
bool EpaperSpectra6133::initialize() {
  ESP_LOGI(TAG, "Initializing (BUSY=%d)", this->transport_.busy_level());
  if (!this->transport_.init_gpio()) {
    return false;
  }
  if (!this->transport_.init_spi()) {
    return false;
  }

  this->transport_.set_load_switch(1);
  this->transport_.set_cs_all(1);
  ESP_LOGD(TAG, "LOAD_SW active, CS idle, BUSY before reset=%d", this->transport_.busy_level());

  if (!this->controller_.init_panel()) {
    return false;
  }

  this->sleeping_ = false;
  ESP_LOGI(TAG, "Display initialized");
  return true;
}

void EpaperSpectra6133::clear() { fill_buffer_with_code(this->buffer_, color_to_code(this->clear_color_)); }

/**
 * @brief Schedules a partial update of a logical rectangle and returns immediately.
 *
 * Re-runs the display lambda, then transfers and refreshes only the specified
 * region cooperatively from loop().
 *
 * If another display operation is already in progress it is superseded.
 */
void EpaperSpectra6133::update_region(int x, int y, int width, int height) {
  ESP_LOGD(TAG, "Region update requested: x=%d y=%d w=%d h=%d", x, y, width, height);
  this->request_display_operation_(DisplayOperationType::UPDATE_REGION, x, y, width, height, "update_region");
}

void EpaperSpectra6133::update_region(UpdateRegion region) {
  this->update_region(region.x, region.y, region.width, region.height);
}

/**
 * @brief Schedules a full framebuffer refresh and returns immediately.
 *
 * The display lambda is NOT re-run.  Existing framebuffer contents are
 * transferred and the panel is refreshed cooperatively from loop().
 *
 * If another display operation is already in progress it is superseded.
 */
void EpaperSpectra6133::refresh() {
  ESP_LOGD(TAG, "Refresh requested");
  this->request_display_operation_(DisplayOperationType::REFRESH, 0, 0, 0, 0, "refresh");
}

void EpaperSpectra6133::flush() {
  ESP_LOGW(TAG, "flush() is deprecated and will be removed in version 1.0; use refresh()");
  this->refresh();
}

/**
 * @brief Schedules a partial framebuffer refresh and returns immediately.
 *
 * The display lambda is NOT re-run.  Only the specified region is transferred
 * and refreshed cooperatively from loop().
 *
 * If another display operation is already in progress it is superseded.
 */
void EpaperSpectra6133::refresh_region(int x, int y, int width, int height) {
  ESP_LOGD(TAG, "Refresh region requested: x=%d y=%d w=%d h=%d", x, y, width, height);
  this->request_display_operation_(DisplayOperationType::REFRESH_REGION, x, y, width, height, "refresh_region");
}

void EpaperSpectra6133::flush_region(int x, int y, int width, int height) {
  ESP_LOGW(TAG, "flush_region() is deprecated and will be removed in version 1.0; use refresh_region()");
  this->refresh_region(x, y, width, height);
}

bool EpaperSpectra6133::is_busy() const {
  ESP_LOGW(TAG, "is_busy() is deprecated and will be removed in version 1.0; use is_processing()");
  return this->is_processing();
}

void EpaperSpectra6133::sleep() {
  ESP_LOGD(TAG, "Deep sleep requested");
  if (this->is_failed()) {
    ESP_LOGE(TAG, "Display not ready, cannot schedule sleep");
    return;
  }
  if (this->sleeping_) {
    return;
  }
  if (!this->controller_.is_initialized() && !this->initialize()) {
    ESP_LOGE(TAG, "Display not ready, cannot schedule sleep");
    return;
  }
  this->schedule_display_operation_(DisplayOperationType::SLEEP, 0, 0, 0, 0);
}

bool EpaperSpectra6133::wake() {
  if (this->is_failed()) {
    return false;
  }
  if (this->controller_.is_initialized() && !this->sleeping_) {
    return true;
  }
  ESP_LOGD(TAG, "Waking display from deep sleep");
  return this->initialize();
}

// ---------------------------------------------------------------------------
// Change detection API
// ---------------------------------------------------------------------------

UpdateRegion EpaperSpectra6133::detect_changed_region() const {
  if (this->change_detection_mode_ == ChangeDetectionMode::COMPARE && this->previous_frame_buffer_ != nullptr &&
      this->buffer_ != nullptr) {
    return find_changed_region(this->buffer_, this->previous_frame_buffer_);
  }
  return this->tracked_region_;
}

void EpaperSpectra6133::reset_change_tracking() { this->tracked_region_ = {}; }

/** @brief Writes a single pixel at (x, y) into the framebuffer.
 *
 * Called by ESPHome's drawing primitives (lines, shapes, text, images) for
 * every pixel they generate.  Out-of-bounds coordinates and a null buffer
 * are silently ignored.  The actual panel update happens later in update().
 *
 * @param x      Logical pixel column (0 = left edge).
 * @param y      Logical pixel row (0 = top edge).
 * @param color  ESPHome RGB color; mapped to the nearest 4-bit EPD nibble.
 */
void HOT EpaperSpectra6133::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT || this->buffer_ == nullptr) {
    return;
  }

  write_pixel_to_buffer(this->buffer_, x, y, color_to_code(color));
  if (++this->draw_pixels_since_yield_ >= DRAW_PIXELS_PER_DELAY) {
    this->draw_pixels_since_yield_ = 0;
    App.feed_wdt();
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  if (this->change_detection_mode_ == ChangeDetectionMode::TRACK ||
      (this->change_detection_mode_ == ChangeDetectionMode::COMPARE && this->previous_frame_buffer_ == nullptr)) {
    if (!this->tracked_region_.empty()) {
      const int rx = this->tracked_region_.x;
      const int ry = this->tracked_region_.y;
      const int rw = this->tracked_region_.width;
      const int rh = this->tracked_region_.height;
      if (x >= rx && x < rx + rw && y >= ry && y < ry + rh) {
        return;
      }
      const int nx = x < rx ? x : rx;
      const int ny = y < ry ? y : ry;
      this->tracked_region_.x = nx;
      this->tracked_region_.y = ny;
      this->tracked_region_.width = (x >= rx + rw ? x + 1 : rx + rw) - nx;
      this->tracked_region_.height = (y >= ry + rh ? y + 1 : ry + rh) - ny;
    } else {
      this->tracked_region_ = {x, y, 1, 1};
    }
  }
}

/**
 * @brief Ensures the display is initialized before any operation that requires it.
 *
 * Returns false immediately if the component is in a failed state (no retry).
 * Returns true if already initialized — this is safe to call even during an
 * active hardware stage, because no SPI commands are issued.
 * Returns false (deferring wake/initialize) when a hardware stage is in progress;
 * wake() and initialize() must not send SPI commands until the panel is ready.
 * is_in_hw_refresh_stage_() includes the timer-based POST_REFRESH_DELAY settle
 * window even though that stage is not represented by BUSY-pin polling.
 */
bool EpaperSpectra6133::ensure_initialized_() {
  // Guard against infinite re-initialization: once mark_failed() has been called,
  // is_failed() returns true permanently and we must not retry.
  if (this->is_failed()) {
    return false;
  }
  if (this->controller_.is_initialized()) {
    return true;
  }
  // Do not call wake() or initialize() while a hardware stage is in progress;
  // the panel cannot accept SPI commands until the stage has elapsed.
  if (this->is_in_hw_refresh_stage_()) {
    return false;
  }
  if (this->sleeping_) {
    return this->wake();
  }
  return this->initialize();
}

/**
 * @brief Copies sent pixels from the framebuffer into the previous-frame buffer (compare mode).
 *
 * For full-frame operations the entire buffer is copied. For region operations
 * only the clipped controller byte windows that were actually sent to the panel
 * are updated; pixels outside the refreshed windows are left as-is so that
 * find_changed_region() still sees them as pending on the next cycle.
 */
void EpaperSpectra6133::update_previous_frame_() {
  if (this->buffer_ == nullptr) {
    return;
  }
  if (this->change_detection_mode_ != ChangeDetectionMode::COMPARE) {
    return;
  }
  if (this->previous_frame_buffer_ == nullptr) {
    this->previous_frame_buffer_ =
        static_cast<uint8_t *>(heap_caps_malloc(FULL_FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (this->previous_frame_buffer_ == nullptr) {
      ESP_LOGW(TAG, "Previous-frame buffer allocation failed — falling back to track mode");
      this->change_detection_mode_ = ChangeDetectionMode::TRACK;
      return;
    }
    ESP_LOGI(TAG, "Previous-frame buffer allocated (%u bytes, PSRAM)", static_cast<unsigned>(FULL_FRAME_SIZE));
  }
  if (this->active_operation_.use_full_frame) {
    std::memcpy(this->previous_frame_buffer_, this->buffer_, FULL_FRAME_SIZE);
  } else {
    for (int i = 0; i < 2; i++) {
      if (!this->active_operation_.has_region[i]) {
        continue;
      }
      const PartialRegion &region = this->active_operation_.regions[i];
      for (int row = 0; row < static_cast<int>(region.height); row++) {
        const size_t offset =
            (static_cast<size_t>(region.y_start) + static_cast<size_t>(row)) * ROW_BYTES + region.row_byte_offset;
        std::memcpy(this->previous_frame_buffer_ + offset, this->buffer_ + offset, region.row_byte_count);
      }
    }
  }
}
// =============================================================================
// Cooperative display pipeline
// =============================================================================

/**
 * @brief Progresses the active display operation by one bounded step.
 *
 * Called every ESPHome main-loop cycle.  Returns immediately when no display
 * operation is active so the overhead during idle periods is negligible.
 *
 * When an operation is in CANCELLING state while the panel is physically busy
 * (for example, in the middle of a DRF refresh), this method keeps polling until BUSY is
 * released before tearing down hardware state. It also lets POST_REFRESH_DELAY
 * finish before cleanup, because that settle window is SPI-unsafe even though
 * BUSY has already returned high.
 */
void EpaperSpectra6133::loop() {
  if (this->active_operation_.state == DisplayOperationState::IDLE) {
    return;
  }
  if (this->active_operation_.state == DisplayOperationState::CANCELLING) {
    // If the panel is physically committed to a refresh (BUSY pin LOW), wait
    // for it to complete before tearing down.  Calling disable_partial_regions()
    // or starting a new operation while BUSY is asserted is undefined per the datasheet.
    if (this->is_in_hw_refresh_stage_()) {
      if (this->active_operation_.stage == DisplayOperationStage::POST_REFRESH_DELAY) {
        const int64_t delay_us =
            this->active_operation_.use_full_frame ? POST_FULL_REFRESH_DELAY_US : POST_REGION_REFRESH_DELAY_US;
        if ((esp_timer_get_time() - this->active_operation_.stage_start_us) < delay_us) {
          return;  // settle window still active; poll again next loop()
        }
      } else if (this->controller_.is_display_busy()) {
        return;  // panel still busy; poll again next loop()
      }
    }
    this->abort_display_operation_();
    return;
  }
  this->process_display_operation_step_();
}

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------

/**
 * @brief Cancels the current or pending display operation.
 *
 * If no operation is active and no operation is pending, this is a safe no-op.
 * Cancellation takes effect at the next loop() call boundary (i.e. after the
 * current in-flight SPI transaction completes).  CS pins and partial-region
 * state are cleaned up in the following loop() invocation.
 * If the panel is physically in a hardware refresh stage or post-refresh settle
 * window, the operation is marked as CANCELLING and loop() drains it safely once
 * the stage has completed.
 */
void EpaperSpectra6133::cancel() {
  this->pending_operation_ = {};  // discard any operation waiting behind the active one
  if (this->active_operation_.state == DisplayOperationState::IDLE) {
    this->disable_loop();
    return;
  }
  this->active_operation_.state = DisplayOperationState::CANCELLING;
}

// ---------------------------------------------------------------------------
// Display operation management helpers
// ---------------------------------------------------------------------------

/**
 * @brief Schedules a display operation after readiness checks or safe deferral.
 *
 * Requests that arrive while a refresh or post-refresh settle stage is
 * uninterruptible must be queued before attempting wake()/initialize(); an
 * auto-sleeping panel may already have reset controller state, but it still
 * cannot accept SPI traffic until the active operation drains.
 */
void EpaperSpectra6133::request_display_operation_(DisplayOperationType type, int x, int y, int w, int h,
                                                   const char *name) {
  if (this->active_operation_.state != DisplayOperationState::IDLE && this->is_in_hw_refresh_stage_()) {
    this->schedule_display_operation_(type, x, y, w, h);
    return;
  }
  if (!this->ensure_initialized_()) {
    ESP_LOGE(TAG, "Display not ready, cannot schedule %s", name);
    return;
  }
  this->schedule_display_operation_(type, x, y, w, h);
}

/**
 * @brief Cancels any active display operation and initialises a new one of @p type.
 *
 * If the active operation is in a hardware-refresh stage or post-refresh settle
 * window, the hardware cannot be interrupted safely. In that case the active
 * operation is marked cancelled so loop() can drain it gracefully, and the new
 * request is stored as a pending operation. abort_display_operation_() will start
 * the pending operation automatically once the draining refresh has completed.
 *
 * For all other stages (data-transfer or idle) the active operation is aborted
 * inline and the new operation starts immediately.
 */
void EpaperSpectra6133::schedule_display_operation_(DisplayOperationType type, int x, int y, int w, int h) {
  if (this->active_operation_.state != DisplayOperationState::IDLE) {
    if (this->is_in_hw_refresh_stage_()) {
      // Panel is physically committed to a refresh or settle window; we cannot
      // abort inline. Queue the request so it starts after the stage drains.
      ESP_LOGD(TAG, "Display operation deferred: panel in refresh stage, will start after drain");
      this->active_operation_.state = DisplayOperationState::CANCELLING;
      this->pending_operation_.has_pending = true;
      this->pending_operation_.type = type;
      this->pending_operation_.x = x;
      this->pending_operation_.y = y;
      this->pending_operation_.w = w;
      this->pending_operation_.h = h;
      return;
    }
    // Safe to abort inline: CS can be released, no refresh in progress.
    ESP_LOGD(TAG, "Cancelled active display operation");
    this->abort_display_operation_();
  }
  this->active_operation_ = {};
  this->active_operation_.type = type;
  this->active_operation_.stage = DisplayOperationStage::INIT;
  this->active_operation_.state = DisplayOperationState::RUNNING;
  this->active_operation_.region_x = x;
  this->active_operation_.region_y = y;
  this->active_operation_.region_width = w;
  this->active_operation_.region_height = h;
  this->enable_loop();
  ESP_LOGD(TAG, "Scheduled display operation (type=%d)", static_cast<int>(type));
}

/**
 * @brief Cleans up hardware state after a cancelled operation and clears active_operation_.
 *
 * Always releases all CS pins and disables partial-region mode so the
 * display controller is left in a known-safe state for the next operation,
 * regardless of which stage was interrupted. By the time this is called, BUSY
 * has already been released and any POST_REFRESH_DELAY settle window has elapsed,
 * so sending disable_partial_regions() via SPI is safe.
 *
 * If a pending operation was queued while we were draining a refresh stage, it
 * is started immediately so is_processing() remains true without a gap.
 */
void EpaperSpectra6133::abort_display_operation_() {
  this->controller_.end_half_transfer();  // safe to call even if CS was already high
  if (!this->sleeping_) {
    this->controller_.disable_partial_regions();  // safe: BUSY has cleared before we reach here
  }
  this->active_operation_ = {};
  ESP_LOGD(TAG, "Display operation aborted");

  // Start any pending operation that was queued while we were draining a refresh.
  if (this->pending_operation_.has_pending) {
    PendingDisplayOperation p = this->pending_operation_;
    this->pending_operation_ = {};
    this->active_operation_.type = p.type;
    this->active_operation_.stage = DisplayOperationStage::INIT;
    this->active_operation_.state = DisplayOperationState::RUNNING;
    this->active_operation_.region_x = p.x;
    this->active_operation_.region_y = p.y;
    this->active_operation_.region_width = p.w;
    this->active_operation_.region_height = p.h;
    ESP_LOGD(TAG, "Started pending display operation (type=%d)", static_cast<int>(p.type));
  } else {
    this->disable_loop();
  }
}

/**
 * @brief Finalises a completed display operation: cleanup, update previous frame, reset tracking, and clear.
 *
 * For region operations, only the refreshed rectangle is synced into the previous-frame buffer
 * (compare mode) and dirty tracking is cleared only when the refreshed rect fully contains the
 * current tracked region (track mode).  This preserves pending changes outside the refreshed
 * area so the next partial update does not skip pixels the panel never received.
 */
void EpaperSpectra6133::finish_display_operation_() {
  if (!this->active_operation_.use_full_frame && !this->sleeping_) {
    this->controller_.disable_partial_regions();
  }
  this->update_previous_frame_();
  if (this->active_operation_.use_full_frame) {
    this->reset_change_tracking();
  } else {
    const UpdateRegion &tr = this->tracked_region_;
    if (partial_regions_contain_tracked_region(this->active_operation_.regions, this->active_operation_.has_region,
                                               tr)) {
      this->reset_change_tracking();
    }
  }
  this->active_operation_ = {};
  this->disable_loop();
  ESP_LOGD(TAG, "Display operation completed");
}

// ---------------------------------------------------------------------------
// Display operation step dispatcher
// ---------------------------------------------------------------------------

/**
 * @brief Returns true if the current operation stage may have the panel physically busy.
 *
 * Stages from POWER_ON through WAIT_POWER_OFF encompass the panel's power-on,
 * display-refresh, and power-off sequence. During these stages the BUSY pin may
 * be LOW and SPI commands must not be sent until it goes HIGH again.
 * POST_REFRESH_DELAY is also included because the panel still needs a
 * timing-sensitive settle window before cleanup or replacement SPI traffic.
 */
bool EpaperSpectra6133::is_in_hw_refresh_stage_() const {
  switch (this->active_operation_.stage) {
    case DisplayOperationStage::POWER_ON:
    case DisplayOperationStage::WAIT_POWER_ON:
    case DisplayOperationStage::PRE_REFRESH_DELAY:
    case DisplayOperationStage::REFRESH_SCREEN:
    case DisplayOperationStage::WAIT_REFRESH:
    case DisplayOperationStage::POWER_OFF:
    case DisplayOperationStage::WAIT_POWER_OFF:
    case DisplayOperationStage::DEEP_SLEEP:
    case DisplayOperationStage::POST_REFRESH_DELAY:
      return true;
    default:
      return false;
  }
}

void EpaperSpectra6133::process_display_operation_step_() {
  switch (this->active_operation_.stage) {
    case DisplayOperationStage::INIT:
      this->process_init_stage_();
      break;
    case DisplayOperationStage::TRANSFER_LEFT_HALF:
      this->process_transfer_left_half_stage_();
      break;
    case DisplayOperationStage::TRANSFER_RIGHT_HALF:
      this->process_transfer_right_half_stage_();
      break;
    case DisplayOperationStage::TRANSFER_LEFT_REGION:
      this->process_transfer_left_region_stage_();
      break;
    case DisplayOperationStage::TRANSFER_RIGHT_REGION:
      this->process_transfer_right_region_stage_();
      break;
    case DisplayOperationStage::ARM_UNCHANGED_HALVES:
      this->process_arm_unchanged_halves_stage_();
      break;
    case DisplayOperationStage::POWER_ON:
      this->process_power_on_stage_();
      break;
    case DisplayOperationStage::WAIT_POWER_ON:
      this->process_wait_power_on_stage_();
      break;
    case DisplayOperationStage::PRE_REFRESH_DELAY:
      this->process_pre_refresh_delay_stage_();
      break;
    case DisplayOperationStage::REFRESH_SCREEN:
      this->process_refresh_screen_stage_();
      break;
    case DisplayOperationStage::WAIT_REFRESH:
      this->process_wait_refresh_stage_();
      break;
    case DisplayOperationStage::POWER_OFF:
      this->process_power_off_stage_();
      break;
    case DisplayOperationStage::WAIT_POWER_OFF:
      this->process_wait_power_off_stage_();
      break;
    case DisplayOperationStage::DEEP_SLEEP:
      this->process_deep_sleep_stage_();
      break;
    case DisplayOperationStage::POST_REFRESH_DELAY:
      this->process_post_refresh_delay_stage_();
      break;
    case DisplayOperationStage::FINISHING:
      this->finish_display_operation_();
      break;
  }
}

// ---------------------------------------------------------------------------
// Per-stage step handlers
// ---------------------------------------------------------------------------

/**
 * @brief INIT: run the display lambda if needed; resolve full-frame vs region path.
 *
 * For UPDATE and UPDATE_REGION the display lambda is called here (fast,
 * synchronous — the lambda just writes pixels into the framebuffer).
 * For UPDATE with refresh_mode PARTIAL, the changed region is detected and the
 * operation can complete immediately if no pixels changed.
 * Computes PartialRegion descriptors for region-path operations.
 */
void EpaperSpectra6133::process_init_stage_() {
  if (this->active_operation_.type == DisplayOperationType::SLEEP) {
    this->active_operation_.stage = DisplayOperationStage::DEEP_SLEEP;
    return;
  }

  if (!this->ensure_initialized_()) {
    ESP_LOGE(TAG, "Display not ready, aborting display operation");
    this->abort_display_operation_();
    return;
  }

  const bool needs_lambda = (this->active_operation_.type == DisplayOperationType::UPDATE ||
                             this->active_operation_.type == DisplayOperationType::UPDATE_REGION);

  if (needs_lambda) {
    // In track mode, reset the accumulator before running the lambda so we
    // capture only the pixels drawn during this update cycle.
    if (this->change_detection_mode_ == ChangeDetectionMode::TRACK) {
      this->reset_change_tracking();
    }
    this->do_update_();
  }

  // For UPDATE with partial update mode, detect the changed region and
  // potentially skip the hardware refresh entirely if nothing changed.
  if (this->active_operation_.type == DisplayOperationType::UPDATE && this->refresh_mode_ == RefreshMode::PARTIAL) {
    const UpdateRegion changed = this->detect_changed_region();
    if (changed.empty()) {
      ESP_LOGI(TAG, "No changes detected — skipping refresh operation");
      this->active_operation_ = {};  // clear without calling finish_display_operation_ (no hardware to clean up)
      this->disable_loop();
      return;
    }
    // If the changed region covers the full display, use full-frame path.
    if (changed.x <= 0 && changed.y <= 0 && changed.width >= EPD_WIDTH && changed.height >= EPD_HEIGHT) {
      this->active_operation_.use_full_frame = true;
    } else {
      // Use the detected rectangle for the region transfer path.
      this->active_operation_.region_x = changed.x;
      this->active_operation_.region_y = changed.y;
      this->active_operation_.region_width = changed.width;
      this->active_operation_.region_height = changed.height;
      this->active_operation_.use_full_frame = false;
    }
  } else if (this->active_operation_.type == DisplayOperationType::UPDATE_REGION ||
             this->active_operation_.type == DisplayOperationType::REFRESH_REGION) {
    this->active_operation_.use_full_frame = false;
  } else {
    // UPDATE in full refresh mode and REFRESH use the full-frame path.
    this->active_operation_.use_full_frame = true;
  }

  if (this->active_operation_.use_full_frame) {
    this->active_operation_.current_row = 0;
    this->active_operation_.stage = DisplayOperationStage::TRANSFER_LEFT_HALF;
  } else {
    this->controller_.compute_partial_regions(
        this->active_operation_.region_x, this->active_operation_.region_y, this->active_operation_.region_width,
        this->active_operation_.region_height, this->active_operation_.regions, this->active_operation_.has_region);

    if (!this->active_operation_.has_region[0] && !this->active_operation_.has_region[1]) {
      ESP_LOGW(TAG, "Partial refresh operation skipped: rectangle outside panel");
      this->active_operation_ = {};
      this->disable_loop();
      return;
    }
    this->active_operation_.current_row = 0;
    this->active_operation_.stage = DisplayOperationStage::TRANSFER_LEFT_REGION;
  }
}

/**
 * @brief TRANSFER_LEFT_HALF: stream OPERATION_ROWS_PER_STEP rows from the left IC0 half.
 *
 * On the first call (current_row == 0) the DTM command is sent to IC0 before
 * streaming begins.  CS remains LOW across all calls for this stage.
 * On completion CS is released and the stage advances to TRANSFER_RIGHT_HALF.
 */
void EpaperSpectra6133::process_transfer_left_half_stage_() {
  if (this->active_operation_.current_row == 0) {
    if (!this->controller_.begin_half_transfer(0)) {
      ESP_LOGE(TAG, "Left-half transfer begin failed");
      this->abort_display_operation_();
      return;
    }
  }

  const int remaining = EPD_HEIGHT - this->active_operation_.current_row;
  const int to_send = remaining < OPERATION_ROWS_PER_STEP ? remaining : OPERATION_ROWS_PER_STEP;
  for (int i = 0; i < to_send; i++) {
    if (!this->controller_.write_half_row(this->buffer_, this->active_operation_.current_row, 0)) {
      ESP_LOGE(TAG, "Left-half transfer row %d write failed", this->active_operation_.current_row);
      this->abort_display_operation_();
      return;
    }
    this->active_operation_.current_row++;
  }

  if (this->active_operation_.current_row >= EPD_HEIGHT) {
    this->controller_.end_half_transfer();
    this->active_operation_.current_row = 0;
    this->active_operation_.stage = DisplayOperationStage::TRANSFER_RIGHT_HALF;
  }
}

/**
 * @brief TRANSFER_RIGHT_HALF: stream OPERATION_ROWS_PER_STEP rows from the right IC1 half.
 *
 * Mirror of TRANSFER_LEFT_HALF for IC1.  On completion transitions to POWER_ON.
 */
void EpaperSpectra6133::process_transfer_right_half_stage_() {
  if (this->active_operation_.current_row == 0) {
    if (!this->controller_.begin_half_transfer(1)) {
      ESP_LOGE(TAG, "Right-half transfer begin failed");
      this->abort_display_operation_();
      return;
    }
  }

  const int remaining = EPD_HEIGHT - this->active_operation_.current_row;
  const int to_send = remaining < OPERATION_ROWS_PER_STEP ? remaining : OPERATION_ROWS_PER_STEP;
  for (int i = 0; i < to_send; i++) {
    if (!this->controller_.write_half_row(this->buffer_, this->active_operation_.current_row, 1)) {
      ESP_LOGE(TAG, "Right-half transfer row %d write failed", this->active_operation_.current_row);
      this->abort_display_operation_();
      return;
    }
    this->active_operation_.current_row++;
  }

  if (this->active_operation_.current_row >= EPD_HEIGHT) {
    this->controller_.end_half_transfer();
    this->active_operation_.current_row = 0;
    this->active_operation_.stage = DisplayOperationStage::POWER_ON;
  }
}

/** @brief TRANSFER_LEFT_REGION: stream region rows for IC0 (skipped instantly if !has_region[0]). */
void EpaperSpectra6133::process_transfer_left_region_stage_() {
  if (!this->active_operation_.has_region[0]) {
    this->active_operation_.current_row = 0;
    this->active_operation_.stage = DisplayOperationStage::TRANSFER_RIGHT_REGION;
    return;
  }

  const PartialRegion &region = this->active_operation_.regions[0];

  if (this->active_operation_.current_row == 0) {
    if (!this->controller_.begin_region_transfer(region)) {
      ESP_LOGE(TAG, "Left-region transfer begin failed");
      this->abort_display_operation_();
      return;
    }
  }

  const int remaining = static_cast<int>(region.height) - this->active_operation_.current_row;
  const int to_send = remaining < OPERATION_ROWS_PER_STEP ? remaining : OPERATION_ROWS_PER_STEP;
  for (int i = 0; i < to_send; i++) {
    if (!this->controller_.write_region_row(this->buffer_, region, this->active_operation_.current_row)) {
      ESP_LOGE(TAG, "Left-region transfer row %d write failed", this->active_operation_.current_row);
      this->controller_.end_region_transfer(region.cs_index);
      this->abort_display_operation_();
      return;
    }
    this->active_operation_.current_row++;
  }

  if (this->active_operation_.current_row >= static_cast<int>(region.height)) {
    this->controller_.end_region_transfer(region.cs_index);
    this->active_operation_.current_row = 0;
    this->active_operation_.stage = DisplayOperationStage::TRANSFER_RIGHT_REGION;
  }
}

/** @brief TRANSFER_RIGHT_REGION: stream region rows for IC1 (skipped instantly if !has_region[1]). */
void EpaperSpectra6133::process_transfer_right_region_stage_() {
  if (!this->active_operation_.has_region[1]) {
    this->active_operation_.current_row = 0;
    this->active_operation_.stage = DisplayOperationStage::ARM_UNCHANGED_HALVES;
    return;
  }

  const PartialRegion &region = this->active_operation_.regions[1];

  if (this->active_operation_.current_row == 0) {
    if (!this->controller_.begin_region_transfer(region)) {
      ESP_LOGE(TAG, "Right-region transfer begin failed");
      this->abort_display_operation_();
      return;
    }
  }

  const int remaining = static_cast<int>(region.height) - this->active_operation_.current_row;
  const int to_send = remaining < OPERATION_ROWS_PER_STEP ? remaining : OPERATION_ROWS_PER_STEP;
  for (int i = 0; i < to_send; i++) {
    if (!this->controller_.write_region_row(this->buffer_, region, this->active_operation_.current_row)) {
      ESP_LOGE(TAG, "Right-region transfer row %d write failed", this->active_operation_.current_row);
      this->controller_.end_region_transfer(region.cs_index);
      this->abort_display_operation_();
      return;
    }
    this->active_operation_.current_row++;
  }

  if (this->active_operation_.current_row >= static_cast<int>(region.height)) {
    this->controller_.end_region_transfer(region.cs_index);
    this->active_operation_.current_row = 0;
    this->active_operation_.stage = DisplayOperationStage::ARM_UNCHANGED_HALVES;
  }
}

/** @brief ARM_UNCHANGED_HALVES: arm a minimal 16x2 PTLW on every IC that had no pixel changes. */
void EpaperSpectra6133::process_arm_unchanged_halves_stage_() {
  for (uint8_t ic = 0; ic < 2; ic++) {
    if (this->active_operation_.has_region[ic]) {
      continue;
    }
    if (!this->controller_.arm_dummy_region(ic)) {
      ESP_LOGE(TAG, "Unchanged-half arm IC%u failed", ic);
      this->abort_display_operation_();
      return;
    }
  }
  this->active_operation_.stage = DisplayOperationStage::POWER_ON;
}

/** @brief POWER_ON: send the PON command; transitions to WAIT_POWER_ON immediately. */
void EpaperSpectra6133::process_power_on_stage_() {
  if (!this->controller_.send_refresh_pon()) {
    ESP_LOGE(TAG, "Refresh power-on command failed");
    this->abort_display_operation_();
    return;
  }
  this->active_operation_.stage = DisplayOperationStage::WAIT_POWER_ON;
}

/** @brief WAIT_POWER_ON: yield each loop() call until BUSY goes high (panel powered on). */
void EpaperSpectra6133::process_wait_power_on_stage_() {
  if (this->controller_.is_display_busy()) {
    return;  // still busy; try again next loop()
  }
  this->active_operation_.stage_start_us = esp_timer_get_time();
  this->active_operation_.stage = DisplayOperationStage::PRE_REFRESH_DELAY;
}

/** @brief PRE_REFRESH_DELAY: non-blocking 30 ms delay required between PON and DRF. */
void EpaperSpectra6133::process_pre_refresh_delay_stage_() {
  if ((esp_timer_get_time() - this->active_operation_.stage_start_us) < PRE_REFRESH_DELAY_US) {
    return;  // delay not elapsed; try again next loop()
  }
  this->active_operation_.stage = DisplayOperationStage::REFRESH_SCREEN;
}

/** @brief REFRESH_SCREEN: send the DRF command; records start time; transitions to WAIT_REFRESH. */
void EpaperSpectra6133::process_refresh_screen_stage_() {
  if (!this->controller_.send_refresh_drf()) {
    ESP_LOGE(TAG, "Refresh command failed");
    this->abort_display_operation_();
    return;
  }
  this->active_operation_.stage_start_us = esp_timer_get_time();
  this->active_operation_.stage = DisplayOperationStage::WAIT_REFRESH;
}

/**
 * @brief WAIT_REFRESH: yield each loop() call until BUSY goes high (panel refreshed).
 *
 * A full e-paper refresh can take up to ~20 s.  The timeout guard here is
 * intentionally generous so normal refresh cycles always complete.  If the
 * panel is unresponsive the operation is aborted to avoid an infinite loop.
 */
void EpaperSpectra6133::process_wait_refresh_stage_() {
  if (this->controller_.is_display_busy()) {
    if ((esp_timer_get_time() - this->active_operation_.stage_start_us) > REFRESH_BUSY_TIMEOUT_US) {
      ESP_LOGE(TAG, "Refresh timeout — BUSY never released");
      this->abort_display_operation_();
    }
    return;  // still busy; try again next loop()
  }
  this->active_operation_.stage = DisplayOperationStage::POWER_OFF;
}

/** @brief POWER_OFF: send the POF command; transitions to WAIT_POWER_OFF. */
void EpaperSpectra6133::process_power_off_stage_() {
  if (!this->controller_.send_refresh_pof()) {
    ESP_LOGE(TAG, "Refresh power-off command failed");
    this->abort_display_operation_();
    return;
  }
  this->active_operation_.stage = DisplayOperationStage::WAIT_POWER_OFF;
}

/** @brief WAIT_POWER_OFF: yield each loop() call until BUSY goes high (panel powered off). */
void EpaperSpectra6133::process_wait_power_off_stage_() {
  if (this->controller_.is_display_busy()) {
    return;  // still busy; try again next loop()
  }
  this->active_operation_.stage_start_us = esp_timer_get_time();
  this->active_operation_.stage =
      this->auto_sleep_ ? DisplayOperationStage::DEEP_SLEEP : DisplayOperationStage::POST_REFRESH_DELAY;
}

/**
 * @brief DEEP_SLEEP: send DSLP+0xA5 once BUSY is idle-high.
 *
 * Region operations clear PTLW before sleeping because no further SPI writes should
 * follow after the controller has entered deep sleep.
 */
void EpaperSpectra6133::process_deep_sleep_stage_() {
  if (this->controller_.is_display_busy()) {
    return;
  }

  if (!this->active_operation_.use_full_frame) {
    this->controller_.disable_partial_regions();
  }
  if (!this->controller_.send_deep_sleep()) {
    ESP_LOGE(TAG, "Deep sleep command failed");
    this->abort_display_operation_();
    return;
  }

  this->controller_.reset();
  this->sleeping_ = true;
  if (this->power_off_after_sleep_) {
    this->transport_.set_load_switch(0);
  }
  this->active_operation_.stage_start_us = esp_timer_get_time();
  this->active_operation_.stage = this->active_operation_.type == DisplayOperationType::SLEEP
                                      ? DisplayOperationStage::FINISHING
                                      : DisplayOperationStage::POST_REFRESH_DELAY;
}

/**
 * @brief POST_REFRESH_DELAY: non-blocking post-refresh delay before finalisation.
 *
 * Full-frame transfers use a 10 ms delay; region transfers use 300 ms.
 * These match the vTaskDelay() calls in the blocking transfer_full_frame()
 * and transfer_region() implementations.
 */
void EpaperSpectra6133::process_post_refresh_delay_stage_() {
  const int64_t delay_us =
      this->active_operation_.use_full_frame ? POST_FULL_REFRESH_DELAY_US : POST_REGION_REFRESH_DELAY_US;
  if ((esp_timer_get_time() - this->active_operation_.stage_start_us) < delay_us) {
    return;  // delay not elapsed; try again next loop()
  }
  this->active_operation_.stage = DisplayOperationStage::FINISHING;
}

}  // namespace epaper_spectra6_133
}  // namespace esphome
