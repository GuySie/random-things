#pragma once
#include "esp_attr.h"

// Circular buffer for recently displayed images.
// RTC_DATA_ATTR: survives deep sleep, resets on full power cycle.
// Slot value 0 = empty (images are 1-indexed).
RTC_DATA_ATTR int g_history[50];
RTC_DATA_ATTR uint8_t g_hist_head;
RTC_DATA_ATTR uint8_t g_hist_count;
