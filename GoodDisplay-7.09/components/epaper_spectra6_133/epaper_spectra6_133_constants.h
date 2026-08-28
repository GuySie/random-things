#pragma once

/**
 * @file epaper_spectra6_133_constants.h
 * @brief Shared panel geometry, framebuffer sizing, palette values, and command codes.
 *
 * These constants capture the hardware contract used throughout the component:
 * logical resolution, packed framebuffer dimensions, SPI chunk sizing, panel
 * palette encoding, and controller register addresses.
 */

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace epaper_spectra6_133 {

/** Logical panel width in pixels. */
static constexpr int EPD_WIDTH = 1200;
/** Logical panel height in pixels. */
static constexpr int EPD_HEIGHT = 1600;
/** Number of packed framebuffer bytes per logical row. */
static constexpr size_t ROW_BYTES = EPD_WIDTH / 2;
/** Number of packed framebuffer bytes handled by one driver IC per row. */
static constexpr size_t HALF_ROW_BYTES = ROW_BYTES / 2;
/** Total framebuffer size in bytes for one full-screen image. */
static constexpr size_t FULL_FRAME_SIZE = ROW_BYTES * EPD_HEIGHT;
/** Maximum SPI payload chunk sent in one transaction. */
static constexpr size_t SPI_MAX_TRANSFER_SIZE = 32768;

// -----------------------------------------------------------------------
// Colour palette constants.
//
// Each byte stores the 4-bit colour code in BOTH nibbles, e.g.
// COLOR_WHITE = 0x11 means colour code 0x1 replicated as high and low
// nibble.  This allows the same constant to serve as:
//   - a memset fill byte for clearing the entire framebuffer, and
//   - a single-pixel nibble (the low nibble is the raw 4-bit code).
//
// The COLOR_ prefix avoids a name collision with the esphome::Color
// objects of the same short names exposed on EpaperSpectra6133.
// -----------------------------------------------------------------------
static constexpr uint8_t COLOR_BLACK = 0x00;   ///< Colour code 0x0 (black).
static constexpr uint8_t COLOR_WHITE = 0x11;   ///< Colour code 0x1 (white).
static constexpr uint8_t COLOR_YELLOW = 0x22;  ///< Colour code 0x2 (yellow).
static constexpr uint8_t COLOR_RED = 0x33;     ///< Colour code 0x3 (red).
static constexpr uint8_t COLOR_BLUE = 0x55;    ///< Colour code 0x5 (blue).
static constexpr uint8_t COLOR_GREEN = 0x66;   ///< Colour code 0x6 (green).

/** Panel setting register. */
static constexpr uint8_t PSR = 0x00;
/** Power setting register. */
static constexpr uint8_t PWR = 0x01;
/** Power-off command. */
static constexpr uint8_t POF = 0x02;
/** Deep-sleep command (data byte 0xA5 required). Powers off all circuitry. */
static constexpr uint8_t DSLP = 0x07;
/** Power-on command. */
static constexpr uint8_t PON = 0x04;
/** Negative booster soft-start register. */
static constexpr uint8_t BTST_N = 0x05;
/** Positive booster soft-start register. */
static constexpr uint8_t BTST_P = 0x06;
/** Display data transmission command. */
static constexpr uint8_t DTM = 0x10;
/** Display refresh command. */
static constexpr uint8_t DRF = 0x12;
/** VCOM and data interval setting register. */
static constexpr uint8_t CDI = 0x50;
/** Timing controller setting register. */
static constexpr uint8_t TCON = 0x60;
/** Resolution register. */
static constexpr uint8_t TRES = 0x61;
/** Analog timing register. */
static constexpr uint8_t AN_TM = 0x74;
/** Partial window register. */
static constexpr uint8_t PTLW = 0x83;
/** Analog block control register. */
static constexpr uint8_t AGID = 0x86;
/** Negative voltage buck/boost control register. */
static constexpr uint8_t BUCK_BOOST_VDDN = 0xB0;
/** TFT/VCOM power control register. */
static constexpr uint8_t TFT_VCOM_POWER = 0xB1;
/** Internal buffer enable register. */
static constexpr uint8_t EN_BUF = 0xB6;
/** Positive voltage boost enable register. */
static constexpr uint8_t BOOST_VDDP_EN = 0xB7;
/** Clock configuration register. */
static constexpr uint8_t CCSET = 0xE0;
/** Power saving register. */
static constexpr uint8_t PWS = 0xE3;
/** Controller mode register used before partial-window programming. */
static constexpr uint8_t CMD66 = 0xF0;

// -----------------------------------------------------------------------
// Partial-window enable/disable byte (index [8] of the 9-byte PTLW payload).
// -----------------------------------------------------------------------
static constexpr uint8_t PTLW_ENABLE = 0x01;
static constexpr uint8_t PTLW_DISABLE = 0x00;

// -----------------------------------------------------------------------
// Partial-window horizontal alignment constraints.
// These refer to the 9-byte PTLW register encoding AFTER the ×2 pixel-to-
// controller coordinate expansion:
//   HRST[10:0] must be a multiple of 8   — i.e. 8n   (n = 0, 1, 2, …)
//   HRED[10:0] must equal 8m + 7         — i.e. 8m+7 (m = 4, 5, 6, …)
//   HRED − HRST + 1 >= 32                — minimum 32 controller-pixels wide
//   HRED + 1 <= 1200                     — within one IC's 1200-px half
//   (yStart + yLine) must be even        — two-row unit addressing
// build_partial_window() enforces these by aligning to 4-pixel boundaries
// in the logical domain (4 logical px × 2 = 8 controller px).
// -----------------------------------------------------------------------

}  // namespace epaper_spectra6_133
}  // namespace esphome
