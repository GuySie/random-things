#pragma once

/**
 * @file epaper_spectra6_133_framebuffer.h
 * @brief Helpers for mapping logical drawing operations into the packed panel framebuffer.
 */

#include <cstddef>
#include <cstdint>

#include "esphome/core/color.h"

#include "epaper_spectra6_133_constants.h"

namespace esphome {
namespace epaper_spectra6_133 {

/**
 * @brief Maps an ESPHome RGB color to the closest 4-bit panel palette entry.
 *
 * The mapping is heuristic because the physical panel only supports six
 * fixed colours rather than arbitrary RGB output. This function is on the
 * per-pixel draw path, so it computes the six squared RGB distances directly
 * instead of iterating over a palette table.
 *
 * @note Header implementation: this function is called once for every drawn
 *       pixel. Keeping it in the header lets the compiler remove the function
 *       call inside full-frame draw loops. Re-run the benchmarks before moving
 *       this implementation back to the .cpp file.
 */
inline uint8_t color_to_code(const Color &color) {
  const uint32_t rgb = (static_cast<uint32_t>(color.red) << 16) | (static_cast<uint32_t>(color.green) << 8) |
                       static_cast<uint32_t>(color.blue);

  // Exact Spectra 6 colours are common in ESPHome display lambdas and
  // pre-quantized images, so avoid distance calculations for those pixels.
  switch (rgb) {
    case 0x00000000:
      return COLOR_BLACK;
    case 0x00FFFFFF:
      return COLOR_WHITE;
    case 0x00FFFF00:
      return COLOR_YELLOW;
    case 0x00FF0000:
      return COLOR_RED;
    case 0x0000FF00:
      return COLOR_GREEN;
    case 0x000000FF:
      return COLOR_BLUE;
    default:
      break;
  }

  const uint32_t red = color.red;
  const uint32_t green = color.green;
  const uint32_t blue = color.blue;
  const uint32_t inv_red = 255U - red;
  const uint32_t inv_green = 255U - green;
  const uint32_t inv_blue = 255U - blue;

  const uint32_t red_sq = red * red;
  const uint32_t green_sq = green * green;
  const uint32_t blue_sq = blue * blue;
  const uint32_t inv_red_sq = inv_red * inv_red;
  const uint32_t inv_green_sq = inv_green * inv_green;
  const uint32_t inv_blue_sq = inv_blue * inv_blue;

  uint8_t best_code = COLOR_BLACK;
  uint32_t best_distance = red_sq + green_sq + blue_sq;

  uint32_t distance = inv_red_sq + inv_green_sq + inv_blue_sq;
  if (distance < best_distance) {
    best_distance = distance;
    best_code = COLOR_WHITE;
  }

  distance = inv_red_sq + inv_green_sq + blue_sq;
  if (distance < best_distance) {
    best_distance = distance;
    best_code = COLOR_YELLOW;
  }

  distance = inv_red_sq + green_sq + blue_sq;
  if (distance < best_distance) {
    best_distance = distance;
    best_code = COLOR_RED;
  }

  distance = red_sq + green_sq + inv_blue_sq;
  if (distance < best_distance) {
    best_distance = distance;
    best_code = COLOR_BLUE;
  }

  distance = red_sq + inv_green_sq + blue_sq;
  if (distance < best_distance) {
    best_code = COLOR_GREEN;
  }

  return best_code;
}

/**
 * @brief Writes one logical pixel into the nibble-packed framebuffer.
 *
 * Two pixels share one byte: even x in the high nibble, odd x in the low nibble.
 * Callers must ensure buffer != nullptr; all production call sites already do.
 *
 * @param buffer Target framebuffer (must not be null).
 * @param x Logical x coordinate.
 * @param y Logical y coordinate.
 * @param color_code 4-bit panel palette entry.
 *
 * @note Header implementation: this helper is called once for every drawn
 *       pixel. Keeping it in the header lets the compiler remove the function
 *       call from the pixel-writing loop. Re-run the benchmarks before moving
 *       this implementation back to the .cpp file.
 */
inline void write_pixel_to_buffer(uint8_t *buffer, int x, int y, uint8_t color_code) {
  const size_t byte_index = static_cast<size_t>(y) * ROW_BYTES + static_cast<size_t>(x / 2);
  const uint8_t nibble = static_cast<uint8_t>(color_code & 0x0F);
  if ((x & 1) == 0) {
    buffer[byte_index] = static_cast<uint8_t>((buffer[byte_index] & 0x0F) | (nibble << 4));
  } else {
    buffer[byte_index] = static_cast<uint8_t>((buffer[byte_index] & 0xF0) | nibble);
  }
}
/** @brief Fills the whole packed framebuffer with one repeated palette entry. */
void fill_buffer_with_code(uint8_t *buffer, uint8_t color_code);
/** @brief Draws the vendor-style vertical colour bar test pattern into the framebuffer. */
void draw_color_bar(uint8_t *buffer);
/** @brief Draws a tiled six-colour checker pattern into the framebuffer. */
void draw_checkerboard(uint8_t *buffer);

/**
 * @brief Bounding box describing a rectangular region of the display.
 *
 * Used for change detection results and manual partial-update regions.
 * Coordinates are in the logical panel space (x=0 left, y=0 top).
 */
struct UpdateRegion {
  int x{0};
  int y{0};
  int width{0};
  int height{0};

  /** @brief Returns true if the region has zero or negative area. */
  bool empty() const { return width <= 0 || height <= 0; }
};

/**
 * @brief Compares two full framebuffers and returns the bounding box of all differing pixels.
 *
 * The comparison operates on the nibble-packed framebuffer layout and
 * converts the result back to logical panel coordinates. It first locates the
 * changed row range, then narrows to the exact byte-column range inside it.
 *
 * @param current        The newly rendered framebuffer.
 * @param previous_frame The last successfully displayed frame.
 * @return UpdateRegion covering all changed pixels, or an empty region if identical.
 */
UpdateRegion find_changed_region(const uint8_t *current, const uint8_t *previous_frame);

}  // namespace epaper_spectra6_133
}  // namespace esphome
