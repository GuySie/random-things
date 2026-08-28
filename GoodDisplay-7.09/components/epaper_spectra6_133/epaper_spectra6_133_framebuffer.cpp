#include "epaper_spectra6_133_framebuffer.h"

/**
 * @file epaper_spectra6_133_framebuffer.cpp
 * @brief Implements framebuffer colour mapping and reference test patterns.
 */

#include <algorithm>
#include <cstring>

namespace esphome {
namespace epaper_spectra6_133 {

/** @brief Fills every pixel in the framebuffer with the same packed panel colour. */
void fill_buffer_with_code(uint8_t *buffer, uint8_t color_code) {
  if (buffer == nullptr) {
    return;
  }

  const uint8_t packed = static_cast<uint8_t>((color_code << 4) | color_code);
  std::memset(buffer, packed, FULL_FRAME_SIZE);
}

/** @brief Draws six horizontal colour bands matching the vendor demo pattern. */
void draw_color_bar(uint8_t *buffer) {
  if (buffer == nullptr) {
    return;
  }
  static constexpr uint8_t colors[6] = {COLOR_BLACK, COLOR_WHITE, COLOR_YELLOW, COLOR_RED, COLOR_BLUE, COLOR_GREEN};
  const int band_height = EPD_HEIGHT / 6;

  for (int y = 0; y < EPD_HEIGHT; y++) {
    const int band = std::min(5, y / band_height);
    const uint8_t nibble = colors[band];
    const uint8_t packed = static_cast<uint8_t>((nibble << 4) | nibble);
    std::memset(buffer + static_cast<size_t>(y) * ROW_BYTES, packed, ROW_BYTES);
  }
}

/** @brief Draws a repeating six-colour tiled checker pattern across the full panel. */
void draw_checkerboard(uint8_t *buffer) {
  if (buffer == nullptr) {
    return;
  }
  static constexpr uint8_t colors[6] = {COLOR_BLACK, COLOR_WHITE, COLOR_YELLOW, COLOR_RED, COLOR_BLUE, COLOR_GREEN};
  static constexpr int grid_cols = 6;
  static constexpr int grid_rows = 8;
  static constexpr int cell_width = EPD_WIDTH / grid_cols;    // 200
  static constexpr int cell_height = EPD_HEIGHT / grid_rows;  // 200

  for (int y = 0; y < EPD_HEIGHT; y++) {
    const int grid_y = std::min(grid_rows - 1, y / cell_height);
    uint8_t *row_ptr = buffer + static_cast<size_t>(y) * ROW_BYTES;

    // Each cell is 200 px = 100 bytes wide.  Fill cell-by-cell with memset.
    for (int col = 0; col < grid_cols; col++) {
      const uint8_t nibble = colors[(col + grid_y) % 6];
      const uint8_t packed = static_cast<uint8_t>((nibble << 4) | nibble);
      const size_t byte_offset = static_cast<size_t>(col * cell_width / 2);
      const size_t byte_count = static_cast<size_t>(cell_width / 2);
      std::memset(row_ptr + byte_offset, packed, byte_count);
    }
  }
}

/**
 * @brief Compares two full framebuffers byte-by-byte and returns the minimal
 *        bounding box (in logical panel coordinates) of all differing pixels.
 *
 * Byte column N in the framebuffer holds logical pixels 2*N and 2*N+1.
 * After finding the min/max differing byte column and row, the range is
 * converted to logical pixel coordinates directly.
 *
 * Scans from the top and bottom to avoid row-level comparisons once the
 * changed vertical range is known, then narrows to the exact differing
 * byte-column range within that row span.
 */
UpdateRegion find_changed_region(const uint8_t *current, const uint8_t *previous_frame) {
  UpdateRegion rect;

  if (current == nullptr || previous_frame == nullptr) {
    return rect;
  }

  int row_min = 0;
  int row_max = EPD_HEIGHT - 1;
  int col_byte_min = static_cast<int>(ROW_BYTES);
  int col_byte_max = -1;

  while (row_min < EPD_HEIGHT) {
    const size_t row_offset = static_cast<size_t>(row_min) * ROW_BYTES;
    if (std::memcmp(current + row_offset, previous_frame + row_offset, ROW_BYTES) != 0) {
      break;
    }
    row_min++;
  }

  if (row_min == EPD_HEIGHT) {
    // No differences found.
    return rect;
  }

  while (row_max > row_min) {
    const size_t row_offset = static_cast<size_t>(row_max) * ROW_BYTES;
    if (std::memcmp(current + row_offset, previous_frame + row_offset, ROW_BYTES) != 0) {
      break;
    }
    row_max--;
  }

  for (int row = row_min; row <= row_max; row++) {
    const size_t row_offset = static_cast<size_t>(row) * ROW_BYTES;
    const uint8_t *cur_row = current + row_offset;
    const uint8_t *prev_row = previous_frame + row_offset;

    // Scan forward for the first differing byte in this row.
    for (int col = 0; col < static_cast<int>(ROW_BYTES); col++) {
      if (cur_row[col] != prev_row[col]) {
        if (col < col_byte_min) {
          col_byte_min = col;
        }
        break;
      }
    }

    // Scan backward for the last differing byte in this row.
    for (int col = static_cast<int>(ROW_BYTES) - 1; col >= 0; col--) {
      if (cur_row[col] != prev_row[col]) {
        if (col > col_byte_max) {
          col_byte_max = col;
        }
        break;
      }
    }
  }

  // Byte column N contains logical pixels 2*N and 2*N+1.
  const int x_min = col_byte_min * 2;
  const int x_max = col_byte_max * 2 + 1;

  rect.x = x_min;
  rect.y = row_min;
  rect.width = x_max - x_min + 1;
  rect.height = row_max - row_min + 1;

  return rect;
}

}  // namespace epaper_spectra6_133
}  // namespace esphome
