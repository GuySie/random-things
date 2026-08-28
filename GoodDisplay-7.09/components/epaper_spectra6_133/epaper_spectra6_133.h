#pragma once

/**
 * @file epaper_spectra6_133.h
 * @brief ESPHome display component for 13.3-inch Spectra 6 e-paper panels.
 *
 * This header defines the main ESPHome-facing component class that ties together
 * the transport, controller, and framebuffer layers.
 *
 * Panel: 1200×1600 pixels, 6 colours (Black/White/Yellow/Red/Blue/Green),
 * 4 bits per pixel packed into 960 KB of framebuffer.  Two driver ICs each
 * handle 600 columns, accessed via manual chip-select toggling over SPI.
 *
 * @see epaper_spectra6_133_constants.h  — geometry, palette codes, and register addresses
 * @see epaper_spectra6_133_transport.h  — SPI and GPIO transport layer
 * @see epaper_spectra6_133_controller.h — panel register sequencing and refresh logic
 * @see epaper_spectra6_133_framebuffer.h — pixel packing and colour mapping
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "esphome/components/display/display_buffer.h"

#include "epaper_spectra6_133_controller.h"
#include "epaper_spectra6_133_framebuffer.h"
#include "epaper_spectra6_133_version.h"

namespace esphome {
namespace epaper_spectra6_133 {

/** @brief Controls how the component determines which pixels have changed. */
enum class ChangeDetectionMode {
  TRACK,    // Track pixel writes through draw_absolute_pixel_internal.
  COMPARE,  // Compare the current framebuffer against the previous displayed frame.
};

/** @brief Controls whether update() performs full-frame or auto-partial refresh. */
enum class RefreshMode {
  FULL,     // Always transfer the full framebuffer.
  PARTIAL,  // Automatically detect the changed region and refresh only that.
};

/**
 * @brief Deprecated compatibility alias for RefreshMode.
 *
 * @deprecated Use RefreshMode instead. This alias will be removed in version 1.0.
 */
using UpdateMode [[deprecated("Use RefreshMode; UpdateMode will be removed in version 1.0.")]] = RefreshMode;

// ---------------------------------------------------------------------------
// Display operation types and execution stages.
//
// A DisplayOperation describes one scheduled display operation (update, refresh,
// or their region variants). It is progressed from loop() one bounded step at a
// time so that the ESPHome main loop is never blocked for long.
//
// Only one DisplayOperation may be active at any time. Starting a new operation
// while one is running cancels the old operation first.
// ---------------------------------------------------------------------------

/** @brief Identifies which display operation is being performed. */
enum class DisplayOperationType {
  NONE,
  UPDATE,          // Full update: run lambda + full-frame or auto-partial transfer.
  UPDATE_REGION,   // Partial update: run lambda + region transfer.
  REFRESH,         // Full refresh: full-frame transfer without running lambda.
  REFRESH_REGION,  // Partial refresh: region transfer without running lambda.
  SLEEP,           // Enter panel deep sleep without modifying the framebuffer.
};

/**
 * @brief Stage of an in-progress display operation.
 *
 * Stages are progressed in sequence from the component's loop() method.
 * Instantaneous stages (POWER_ON, REFRESH_SCREEN, POWER_OFF) transition immediately;
 * polling stages (WAIT_*) return without advancing until the BUSY pin
 * is released; delay stages (PRE_REFRESH_DELAY, POST_REFRESH_DELAY) use esp_timer_get_time()
 * to achieve non-blocking waits.
 */
enum class DisplayOperationStage {
  // Setup
  INIT,  // Run do_update_() if needed; resolve path; compute PartialRegions.
  // Full-frame transfer
  TRANSFER_LEFT_HALF,   // DTM + row streaming to IC0 (OPERATION_ROWS_PER_STEP rows/call).
  TRANSFER_RIGHT_HALF,  // DTM + row streaming to IC1.
  // Region transfer
  TRANSFER_LEFT_REGION,   // CMD66+PTLW+DTM+rows for IC0 (skipped if !has_region[0]).
  TRANSFER_RIGHT_REGION,  // CMD66+PTLW+DTM+rows for IC1 (skipped if !has_region[1]).
  ARM_UNCHANGED_HALVES,   // Arm dummy PTLWs for ICs with no changed data.
  // Shared refresh sequence
  POWER_ON,            // Send PON command; transition immediately.
  WAIT_POWER_ON,       // Poll is_display_busy(); yield until BUSY goes high.
  PRE_REFRESH_DELAY,   // Non-blocking 30 ms post-PON delay before DRF.
  REFRESH_SCREEN,      // Send DRF command; transition immediately.
  WAIT_REFRESH,        // Poll is_display_busy(); yield until BUSY goes high (up to 20 s).
  POWER_OFF,           // Send POF command; transition immediately.
  WAIT_POWER_OFF,      // Poll is_display_busy(); yield until BUSY goes high.
  DEEP_SLEEP,          // Send DSLP+0xA5 once BUSY is idle-high.
  POST_REFRESH_DELAY,  // Non-blocking post-refresh delay (10 ms full / 300 ms region).
  // Finalization
  FINISHING,  // disable_partial_regions (region operations), update previous frame, clear operation.
};

/** @brief Lifecycle state of a display operation. */
enum class DisplayOperationState {
  IDLE,        // No operation is scheduled or running.
  RUNNING,     // An operation is actively being progressed from loop().
  CANCELLING,  // Operation is marked for cancellation; draining before teardown.
};

/**
 * @brief Internal state for one scheduled display operation.
 *
 * Kept small: only the fields needed to resume the current stage across
 * multiple loop() invocations are stored here.
 */
struct DisplayOperation {
  DisplayOperationType type{DisplayOperationType::NONE};
  DisplayOperationStage stage{DisplayOperationStage::INIT};
  DisplayOperationState state{DisplayOperationState::IDLE};
  // Region parameters (UPDATE_REGION, REFRESH_REGION; also set by auto-partial UPDATE).
  int region_x{0};
  int region_y{0};
  int region_width{0};
  int region_height{0};
  // Resolved at INIT: true -> full-frame stages; false -> region stages.
  bool use_full_frame{true};
  // Computed at INIT for region-path operations.
  PartialRegion regions[2];
  bool has_region[2]{false, false};
  // Row streaming progress (reset to 0 on each FF/RG stage entry).
  int current_row{0};
  // Microsecond timestamp recorded when entering PRE_REFRESH_DELAY or POST_REFRESH_DELAY.
  int64_t stage_start_us{0};
};

/**
 * @brief Holds parameters for an operation requested while the panel
 *        was physically committed to a hardware refresh cycle.
 *
 * When a new operation arrives while the active operation is in a refresh
 * stage and the BUSY pin is still LOW, the new operation cannot start immediately:
 * hardware cannot be interrupted.  The incoming request is stored here and
 * started automatically by abort_display_operation_() once BUSY is released and
 * the draining operation has been torn down.
 *
 * Only one pending operation is kept at a time.  A second replacement overwrites
 * the first, consistent with the existing single-operation-wins semantics.
 */
struct PendingDisplayOperation {
  bool has_pending{false};
  DisplayOperationType type{DisplayOperationType::NONE};
  int x{0};
  int y{0};
  int w{0};
  int h{0};
};

/**
 * ESPHome display component for 13.3-inch Spectra 6 e-paper panels.
 *
 * The component keeps a single full-screen framebuffer in ESPHome's logical
 * coordinate space. The controller layer later remaps each row into the two
 * 600-column driver halves expected by the panel.
 *
 * Lifecycle:
 *   setup()  - allocates the framebuffer and configures GPIO/SPI/EPD.
 *   update() - schedules a full-frame update operation; progressed from loop().
 *   loop()   - progresses the active display operation one bounded step at a time.
 */
class EpaperSpectra6133 : public display::DisplayBuffer {
 public:
  // Panel colour palette constants — the six colours supported by the Spectra 6
  // hardware, expressed as ESPHome Color values for use with any drawing API.
  static constexpr Color BLACK = Color(0, 0, 0);
  static constexpr Color WHITE = Color(255, 255, 255);
  static constexpr Color YELLOW = Color(255, 255, 0);
  static constexpr Color RED = Color(255, 0, 0);
  static constexpr Color BLUE = Color(0, 0, 255);
  static constexpr Color GREEN = Color(0, 255, 0);

  /** @brief Returns the component version string. */
  static const char *get_version() { return VERSION; }

  /**
   * @brief Creates the component and binds the controller to the transport.
   *
   * Pin assignments and the SPI host are supplied later by generated
   * ESPHome configuration code before setup() runs.
   */
  EpaperSpectra6133();

  /** @brief Selects the ESP-IDF SPI host used for panel communication. */
  void set_spi_host(int host) { this->transport_.set_spi_host(host); }
  /** @brief Sets the chip-select pin for the first driver IC. */
  void set_cs0_pin(int pin) { this->transport_.set_cs0_pin(pin); }
  /** @brief Sets the chip-select pin for the second driver IC. */
  void set_cs1_pin(int pin) { this->transport_.set_cs1_pin(pin); }
  /** @brief Sets the SPI clock pin. */
  void set_clk_pin(int pin) { this->transport_.set_clk_pin(pin); }
  /** @brief Sets the primary SPI data output pin connected to DATA0. */
  void set_data0_pin(int pin) { this->transport_.set_data0_pin(pin); }
  /** @brief Sets the secondary SPI data pin connected to DATA1. */
  void set_data1_pin(int pin) { this->transport_.set_data1_pin(pin); }
  /** @brief Sets the active-low BUSY input pin. */
  void set_busy_pin(int pin) { this->transport_.set_busy_pin(pin); }
  /** @brief Sets the hardware reset pin. */
  void set_reset_pin(int pin) { this->transport_.set_reset_pin(pin); }
  /** @brief Sets the external power-supply enable pin for the panel. */
  void set_power_pin(int pin) { this->transport_.set_power_pin(pin); }
  /** @brief Sets the change detection strategy used by detect_changed_region(). */
  void set_change_detection_mode(ChangeDetectionMode mode) { this->change_detection_mode_ = mode; }
  /** @brief Sets whether update() performs full-frame or auto-partial refresh. */
  void set_refresh_mode(RefreshMode mode) { this->refresh_mode_ = mode; }
  /**
   * @brief Deprecated compatibility wrapper for set_refresh_mode().
   *
   * @deprecated Use set_refresh_mode() instead. This method will be removed in version 1.0.
   */
  [[deprecated("Use set_refresh_mode(); set_update_mode() will be removed in version 1.0.")]]
  void set_update_mode(RefreshMode mode);
  /** @brief Sets the colour used by clear() and ESPHome's automatic pre-render clear. */
  void set_clear_color(Color color) { this->clear_color_ = color; }
  /**
   * @brief Sets whether successful refreshes automatically enter deep sleep.
   *
   * @param auto_sleep  True to send the panel deep-sleep command after each
   * successful refresh; false to leave the panel awake until sleep() is called.
   */
  void set_auto_sleep(bool auto_sleep) { this->auto_sleep_ = auto_sleep; }
  /**
   * @brief Sets whether deep sleep also disables the external panel load switch.
   *
   * @param power_off_after_sleep  True to drive power_pin low after the panel
   * enters deep sleep; false to leave the panel power rail enabled.
   */
  void set_power_off_after_sleep(bool power_off_after_sleep) { this->power_off_after_sleep_ = power_off_after_sleep; }

  /** @brief Allocates the framebuffer and configures the hardware. */
  void setup() override;
  /** @brief Logs the active hardware configuration and driver status. */
  void dump_config() override;
  /** @brief Requests hardware-priority setup so the display is ready early in boot. */
  float get_setup_priority() const override;
  /**
   * @brief Schedules a display update and returns immediately.
   *
   * Called automatically by ESPHome on the configured update_interval.
   * Re-runs the display lambda inside loop() and transfers the resulting
   * framebuffer to the panel cooperatively across successive loop() calls.
   *
   * When refresh_mode is PARTIAL, the component detects the changed region
   * and refreshes only that area.  When FULL (default), the entire
   * framebuffer is transferred.
   *
   * If another display operation is already in progress it is superseded:
   * the previous operation is cancelled and this one takes its place.
   */
  void update() override;
  /** @brief Progresses any active display operation by one bounded step. */
  void loop() override;
  /** @brief Fills the logical framebuffer with one ESPHome color without refreshing immediately. */
  void fill(Color color) override;
  /** @brief Reports that this display exposes a multi-colour output surface. */
  display::DisplayType get_display_type() override { return display::DISPLAY_TYPE_COLOR; }

  /** @brief Fills the framebuffer with the configured clear colour without refreshing. */
  void clear() override;

  /**
   * @brief Schedules a partial update of a logical rectangle and returns immediately.
   *
   * Re-runs the display lambda, then transfers and refreshes only the specified
   * region cooperatively from loop().
   *
   * If another display operation is already in progress it is superseded.
   *
   * @param x, y, width, height  Logical panel rectangle (pixels).
   */
  void update_region(int x, int y, int width, int height);

  /**
   * @brief Schedules a partial update using a pre-computed region.
   *
   * Convenience overload that accepts an UpdateRegion value directly.
   * Equivalent to calling update_region(region.x, region.y, region.width, region.height).
   */
  void update_region(UpdateRegion region);

  /**
   * @brief Schedules a full framebuffer refresh and returns immediately.
   *
   * The display lambda is NOT re-run.  Existing framebuffer contents are
   * transferred and the panel is refreshed cooperatively from loop().
   *
   * If another display operation is already in progress it is superseded.
   */
  void refresh();

  /**
   * @brief Deprecated compatibility wrapper for refresh().
   *
   * @deprecated Use refresh() instead. This method will be removed in version 1.0.
   */
  [[deprecated("Use refresh(); flush() will be removed in version 1.0.")]]
  void flush();

  /**
   * @brief Schedules a partial framebuffer refresh and returns immediately.
   *
   * The display lambda is NOT re-run.  Only the specified region is
   * transferred and refreshed cooperatively from loop().
   *
   * If another display operation is already in progress it is superseded.
   *
   * @param x, y, width, height  Logical panel rectangle (pixels).
   */
  void refresh_region(int x, int y, int width, int height);

  /**
   * @brief Deprecated compatibility wrapper for refresh_region().
   *
   * @deprecated Use refresh_region() instead. This method will be removed in version 1.0.
   */
  [[deprecated("Use refresh_region(); flush_region() will be removed in version 1.0.")]]
  void flush_region(int x, int y, int width, int height);

  /** @brief Returns whether the panel hardware has been initialized and is ready for explicit updates. */
  bool is_ready() const { return this->controller_.is_initialized(); }
  /**
   * @brief Returns whether the panel was successfully placed in deep sleep.
   *
   * @return True after the panel has entered deep sleep; false while awake or
   * while a sleep request is still pending.
   */
  bool is_sleeping() const { return this->sleeping_; }

  // -------------------------------------------------------------------------
  // Change detection
  //
  // The component tracks which pixels have changed since the last successful
  // display update.  The detection strategy is selected via
  // change_detection_mode (track or compare).  detect_changed_region() is
  // always available regardless of refresh_mode.
  // -------------------------------------------------------------------------

  /**
   * @brief Returns the detected changed region based on the configured detection mode.
   *
   * In TRACK mode, returns the bounding box of all pixels written through
   * draw_absolute_pixel_internal since the last successful update.
   *
   * In COMPARE mode, compares the current framebuffer against the previous
   * displayed frame and returns the bounding box of all differing pixels.
   *
   * Returns an empty region if no changes are detected.
   */
  UpdateRegion detect_changed_region() const;

  /** @brief Resets the tracked change region to empty. */
  void reset_change_tracking();

  // -------------------------------------------------------------------------
  // Display pipeline control
  //
  // All display operations schedule cooperative work and return immediately.
  // Only one operation may be active at a time. Starting a new operation while one
  // is in progress supersedes it: the previous operation is cancelled and the new
  // one takes its place.  If the panel is physically in a hardware refresh
  // stage, the incoming operation is queued as pending and starts automatically
  // once the refresh drains safely.
  //
  // Use is_processing() to check whether any display operation is scheduled,
  // running, or draining.
  // -------------------------------------------------------------------------

  /**
   * @brief Returns true while any display operation is scheduled, running, or draining.
   *
   * Returns false only when the display pipeline is fully idle: no operation is
   * active and no pending operation is waiting to start.
   */
  bool is_processing() const {
    return this->active_operation_.state != DisplayOperationState::IDLE || this->pending_operation_.has_pending;
  }

  /**
   * @brief Deprecated compatibility wrapper for is_processing().
   *
   * @deprecated Use is_processing() instead. This method will be removed in version 1.0.
   */
  [[deprecated("Use is_processing(); is_busy() will be removed in version 1.0.")]]
  bool is_busy() const;

  /**
   * @brief Cancels the current or pending display operation.
   *
   * If no operation is active and no operation is pending, this is a safe no-op.
   *
   * If an operation is scheduled but not yet physically refreshing the panel, it is
   * cancelled at the next loop() call boundary.
   *
   * If the panel is physically inside a hardware refresh stage (BUSY pin LOW),
   * the current operation is marked as cancelling and drains safely before teardown.
   * Any pending operation queued behind it is also discarded.
   */
  void cancel();

  /**
   * @brief Schedules the panel to enter deep sleep and returns immediately.
   *
   * If a refresh is physically in progress, the sleep request waits until the
   * BUSY line is released before sending the datasheet-defined DSLP command.
   */
  void sleep();

  /**
   * @brief Wakes a sleeping panel by rerunning the full hardware init sequence.
   *
   * @return True when the display is initialized and ready for a new transfer;
   * false if initialization fails or the component is in a failed state.
   */
  bool wake();

 protected:
  /** @brief Returns the logical panel width reported to ESPHome drawing primitives. */
  int get_width_internal() override { return EPD_WIDTH; }
  /** @brief Returns the logical panel height reported to ESPHome drawing primitives. */
  int get_height_internal() override { return EPD_HEIGHT; }
  /** @brief Writes one logical pixel into the packed framebuffer. */
  void draw_absolute_pixel_internal(int x, int y, Color color) override;

 private:
  // Runs the reference initialization sequence once the transport is ready.
  bool initialize();
  // Lazily initializes the panel on the first operation that needs hardware access.
  bool ensure_initialized_();
  // Copies the current framebuffer to the previous-frame buffer (compare mode).
  void update_previous_frame_();

  // -------------------------------------------------------------------------
  // Display operation management helpers.
  // -------------------------------------------------------------------------

  // Validates readiness or queues behind an uninterruptible active operation.
  void request_display_operation_(DisplayOperationType type, int x, int y, int w, int h, const char *name);
  // Cancels any active operation and initialises a new one of the given type.
  void schedule_display_operation_(DisplayOperationType type, int x, int y, int w, int h);
  // Dispatches one bounded step for the current operation stage.
  void process_display_operation_step_();
  // Cleans up CS/PTLW state and clears the operation after cancellation.
  // If a pending operation was queued during a refresh-stage drain, starts it.
  void abort_display_operation_();
  // Cleans up PTLW state, syncs shadow, and clears the operation on completion.
  void finish_display_operation_();
  // Returns true if the current operation stage may have the panel physically busy
  // (BUSY pin potentially LOW).  Cancellation must wait until BUSY clears.
  bool is_in_hw_refresh_stage_() const;

  // Per-stage step handlers — each does a bounded amount of work and returns.
  void process_init_stage_();
  void process_transfer_left_half_stage_();
  void process_transfer_right_half_stage_();
  void process_transfer_left_region_stage_();
  void process_transfer_right_region_stage_();
  void process_arm_unchanged_halves_stage_();
  void process_power_on_stage_();
  void process_wait_power_on_stage_();
  void process_pre_refresh_delay_stage_();
  void process_refresh_screen_stage_();
  void process_wait_refresh_stage_();
  void process_power_off_stage_();
  void process_wait_power_off_stage_();
  void process_deep_sleep_stage_();
  void process_post_refresh_delay_stage_();

  Transport transport_;
  Controller controller_;
  ChangeDetectionMode change_detection_mode_{ChangeDetectionMode::TRACK};
  RefreshMode refresh_mode_{RefreshMode::FULL};
  Color clear_color_{WHITE};
  bool auto_sleep_{true};
  bool power_off_after_sleep_{false};
  bool sleeping_{false};
  UpdateRegion tracked_region_{};
  uint8_t *previous_frame_buffer_{nullptr};
  uint32_t draw_pixels_since_yield_{0};
  DisplayOperation active_operation_{};
  // Holds an operation that arrived while the panel was in a hardware refresh stage.
  // Started automatically after the draining refresh completes.
  PendingDisplayOperation pending_operation_{};

  // Allow the host-test fixture to access private members for state verification.
  // TEST_F subclasses cannot use friend access directly; they call through fixture methods.
  friend class EpaperSpectra6133ComponentTest;
};

}  // namespace epaper_spectra6_133
}  // namespace esphome
