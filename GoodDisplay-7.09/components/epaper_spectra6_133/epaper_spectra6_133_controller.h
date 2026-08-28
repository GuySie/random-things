#pragma once

/**
 * @file epaper_spectra6_133_controller.h
 * @brief Panel controller operations layered on top of the low-level transport.
 */

#include <cstddef>
#include <cstdint>

#include "epaper_spectra6_133_constants.h"
#include "epaper_spectra6_133_transport.h"

namespace esphome {
namespace epaper_spectra6_133 {

/** @brief Describes one aligned partial-update region for a single driver IC. */
struct PartialRegion {
  uint8_t cs_index;
  uint16_t x_start;
  uint16_t y_start;
  uint16_t width;
  uint16_t height;
  size_t row_byte_offset;
  size_t row_byte_count;
};

/**
 * @brief Panel controller that issues register commands through the transport layer.
 *
 * Encapsulates the vendor register sequence and keeps panel-facing rules
 * separate from ESPHome rendering code: register initialisation,
 * full-frame transfer, partial-window setup, refresh, and driver probing.
 */
class Controller {
 public:
  /**
   * @brief Creates a controller facade that issues commands through an existing transport.
   *
   * The transport object owns pin configuration and the SPI device handle.
   */
  explicit Controller(Transport &transport) : transport_(transport) {}

  /** @brief Programs the panel registers after reset and marks the controller initialised. */
  bool init_panel();
  /** @brief Executes the PON, DRF, and POF sequence for the current display memory. */
  bool refresh();
  /** @brief Streams both 600-column controller halves from the packed full framebuffer.
   *  Returns false without issuing transport writes when @p framebuffer is null. */
  bool transfer_full_frame(const uint8_t *framebuffer);
  /**
   * @brief Transfers only the controller-aligned regions touched by a logical rectangle.
   *
   * The supplied rectangle is already in logical panel coordinates. This
   * method splits it into per-controller regions, writes their pixel data,
   * triggers a refresh, and then clears partial-region mode again.
   *
   * Hardware constraint — shared BUSY line
   * ---------------------------------------
   * Both driver ICs are wired to a single BUSY output. After PON, each IC
   * holds BUSY low until it has completed its DRF cycle. If only one IC
   * receives DRF the other never transitions BUSY, causing a timeout.
   * Therefore DRF (via refresh()) is always broadcast to both ICs.
   *
   * To prevent an IC without actual pixel changes from performing a
   * full-screen refresh, a minimal dummy PTLW (16×2 px, no DTM data) is
   * programmed on that IC before DRF. The IC refreshes the tiny window
   * from its existing internal buffer — no visible artefact, but the
   * DRF/BUSY handshake completes normally.
   *
   * Ref: vendor partialWindowUpdateWithoutImageData() uses the same
   * CMD66 → PTLW pattern without subsequent DTM.
   */
  bool transfer_region(const uint8_t *framebuffer, int x, int y, int width, int height);
  /** @brief Reads and logs the vendor status register from both driver ICs. */
  bool probe_drivers();
  /** @brief Reports whether init_panel() has completed successfully. */
  bool is_initialized() const { return this->initialized_; }
  /** @brief Clears the initialised flag so a later caller can re-run panel setup. */
  void reset() { this->initialized_ = false; }

  // Step-based primitives for cooperative display operation execution.
  // begin_half_transfer() and begin_region_transfer() leave one CS line LOW
  // until the matching end_* call. All other methods cycle CS internally.

  /** @brief Selects CS for @p half (0=IC0, 1=IC1) and sends the DTM command.
   *  Returns false without touching CS when @p half is outside the valid range.
   *  CS remains LOW after this call.  Call end_half_transfer() when done. */
  bool begin_half_transfer(uint8_t half);

  /** @brief Streams one row of HALF_ROW_BYTES to the IC selected by @p half.
   *  Returns false without issuing transport writes when @p framebuffer is null,
   *  @p row is outside 0...EPD_HEIGHT-1, or @p half is outside the valid range.
   *  CS must already be LOW (set by begin_half_transfer()). */
  bool write_half_row(const uint8_t *framebuffer, int row, uint8_t half);

  /** @brief Releases all CS pins after a half-transfer streaming phase. */
  void end_half_transfer();

  /** @brief Sends CMD66+PTLW+DTM for @p region.  Returns false without
   *  issuing transport writes when region.cs_index is outside the valid range.
   *  CS for region.cs_index remains LOW after this call.  Call
   *  end_region_transfer() when done. */
  bool begin_region_transfer(const PartialRegion &region);

  /** @brief Streams one row of pixel data for @p region's IC.
   *  Returns false without issuing transport writes when @p framebuffer is null,
   *  region.cs_index is outside the valid range, or @p row is outside
   *  0...region.height-1.  CS must already be LOW (set by
   *  begin_region_transfer()). */
  bool write_region_row(const uint8_t *framebuffer, const PartialRegion &region, int row);

  /** @brief Releases all CS pins after a region streaming phase. */
  void end_region_transfer(uint8_t cs_index);

  /** @brief Programs a minimal 16×2 dummy PTLW on @p cs_index so that a
   *  subsequent DRF does not trigger a full-screen refresh on that IC.
   *  Returns false without issuing transport writes when @p cs_index is
   *  outside the valid range.  No DTM data is sent.  Fully atomic: CS cycles
   *  internally. */
  bool arm_dummy_region(uint8_t cs_index);

  /** @brief Computes aligned PartialRegion descriptors for both ICs from a
   *  logical rectangle.  Wraps the anonymous-namespace build_partial_region()
   *  so that callers outside this translation unit can use the result. */
  void compute_partial_regions(int x, int y, int width, int height, PartialRegion out[2], bool has_region[2]) const;

  /** @brief Sends the PON command (CS all low → write PON → CS all high). */
  bool send_refresh_pon();

  /** @brief Sends the DRF command with its payload (CS all low → write DRF → CS all high). */
  bool send_refresh_drf();

  /** @brief Sends the POF command with its payload (CS all low → write POF → CS all high). */
  bool send_refresh_pof();

  /**
   * @brief Sends the deep-sleep command specified by the panel datasheet.
   *
   * The caller must only invoke this while BUSY is idle-high.  The command is
   * broadcast to both controller halves and requires payload byte 0xA5.
   */
  bool send_deep_sleep();

  /** @brief Returns true while the display is busy (BUSY pin low = panel working). */
  bool is_display_busy() const;

  /** @brief Clears partial-region state so later transfers start from full-screen defaults.
   *  Made public so the FINISHING stage can call it after region operations. */
  void disable_partial_regions();

 private:
  /** @brief Programs the PTLW registers for one already aligned controller region. */
  bool enable_partial_region(const PartialRegion &region);

  Transport &transport_;
  bool initialized_{false};
};

}  // namespace epaper_spectra6_133
}  // namespace esphome
