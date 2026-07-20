# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An ESPHome configuration for using a Seeed reTerminal E1004 (13.3" E Ink Spectra 6 full-color panel, 1200×1600 portrait-native, T133A01 controller) as a digital art frame with Home Assistant. The device is an ESP32-S3R8 (8 MB octal PSRAM, 32 MB flash, 5000 mAh battery) running the esp-idf framework. It wakes from deep sleep on a configurable schedule, downloads a numbered PNG image from Home Assistant's `/local/` folder, renders it, and sleeps again.

This config is a port of the sibling `Seeed-reTerminal-E1003` art-frame config. The E1003 needed a custom IT8951 display component; the E1004's display is supported **upstream** in ESPHome since 2026.7.0 (`display: platform: epaper_spi, model: seeed-reterminal-e1004`, added in esphome/esphome PR #16706, merged 2026-07-04).

**`components/epaper_spi/`** is a verbatim copy of the upstream component from the 2026.7.0 tag with **one bug fix** in `epaper_spi_t133a01.cpp` `transfer_data()` (found on real hardware 2026-07-18): the 10 ms yield check ran *after* the row increment, so when the yield deadline landed on the **final row** of the CS or CS1 phase, the function returned early with the phase complete; on re-entry the phase's `if` block (and its epilogue — `ESP_LOGD("CS[1] phase done")`, `disable()`, CS deassert) was skipped, the transfer reported success with the SPI transaction still open, and the next `enable()` (POWER_ON command) deadlocked the loop task → task watchdog abort → reboot. Intermittent (depends on row-timing alignment per cycle; observed hanging one cycle and passing the next). The fix gates both yield checks with `half < total_rows` / `half < total_rows * 2` so the final row always falls through to the epilogue. Delete this local override once the fix is merged upstream and `min_version` is bumped past it.

## ESPHome references

- **Display driver source**: `esphome/components/epaper_spi/` in https://github.com/esphome/esphome — `epaper_spi_t133a01.{h,cpp}` (T133A01 dual-CS driver), `models/t133a01.py` (the `Seeed-reTerminal-E1004` model definition with all pin defaults).
- **YAML docs**: https://esphome.io — authoritative reference for component options.
- **ESP-IDF API docs**: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/ — for C++ APIs used in lambdas (`RTC_DATA_ATTR`, `esp_random()`, deep sleep).

## Architecture

**`config.yaml`** — the entire device logic lives here. Same flow as the E1003 config: `wifi.on_connect` → charger check + SHT40 + battery read + `random_image` button → sets `image_counter` → `online_image.set_url` → download → `component.update: epaper_display` → deep sleep. Schedule (`Refresh Interval`, `Active Hours Start/End`) is computed clock-aligned from the PCF8563 RTC in the `on_shutdown` priority-100 lambda, identical to the E1003.

Key differences from the E1003 config:

- **Async display refresh / sleep timing** — this is the critical one. The E1003's custom IT8951 component refreshed synchronously inside `update()`, so `deep_sleep.enter` could fire immediately after `component.update`. The upstream `epaper_spi` driver is an **asynchronous state machine** (buffer transfer → init → power on → ~30 s Spectra 6 refresh → power off → panel deep-sleep, advanced from `loop()` with busy-pin waits) and exposes **no YAML-visible "refresh finished" trigger** (`state_` and `is_idle_()` are protected). Entering ESP deep sleep mid-refresh would cut panel power mid-waveform. Solution: `on_download_finished` runs `component.update`, records history, then `delay: ${display_refresh_wait}` (default 60 s, conservative margin over the ~35 s worst case) before the gated `deep_sleep.enter`. The `run_duration` backstop (`deep_sleep_run`, default 180 s) is sized so it also cannot fire mid-refresh in a normal cycle. If tightening these after measuring real timings on hardware, keep `display_refresh_wait` > (transfer + init + refresh + power-off) and `deep_sleep_run` > (worst-case wifi + download + `display_refresh_wait`).
- **Task-watchdog-safe image draw** — the display lambda does NOT use `it.image()`. A full-screen image draw is ~1.92M per-pixel virtual calls into the PSRAM framebuffer and takes >5 s; under esp-idf the task watchdog supervises the loop task (unlike Arduino on the E1003) and **aborts/reboots the device mid-draw** (`task_wdt: ... loopTask ... Aborting.`). This was diagnosed on real hardware 2026-07-18: the stored `esp32.crash` record misleadingly decoded as "Fault - IllegalInstruction at draw_pixel_at" — the abort simply lands wherever the hot loop is. The lambda instead iterates rows itself (row-major, which is also cache-friendlier than `Image::draw`'s column-major loop) and calls `App.feed_wdt()` every 64 rows. Do not "simplify" it back to `it.image()` unless upstream chunks or feeds the WDT in `Display::do_update_`/`Image::draw`.
- **6-color output** — `online_image` uses `type: RGB565` (≈3.8 MB buffer in PSRAM; display's own 4bpp buffer is another ~960 KB — fine on 8 MB). The T133A01 driver maps each drawn `Color` to the 6-color palette via `color_to_index()`: grayscale if max−min < 50 (split at luminance sum 382), else per-channel >128 thresholds → red/green/blue/yellow. Pure palette colors survive RGB565 rounding (255→248 stays >128), so **pre-dithered palette PNGs render exactly**; non-dithered images get hard-snapped and look bad. Images must be dithered to black/white/red/green/blue/yellow on the host (ditherit palette exists for this).
- **Rotation** — panel is portrait-native (1200 wide × 1600 tall). `rotation: 0` = portrait, `90` = landscape (1600×1200 images). The E1003 was the opposite (landscape-native).
- **No temperature injection** — the E1003 fed SHT40 readings to the IT8951 for waveform selection; the T133A01 driver manages this itself. The SHT4x sensor is instead exposed to HA as diagnostics (temperature + humidity), still read once per wake in `wifi.on_connect`.
- **No GPIO hold machinery** — see power notes below.
- **No vcom** — panel supply rails are fixed by the driver's init sequence; nothing panel-specific to configure.

**`image_history.h`** — identical to the E1003's: three `RTC_DATA_ATTR` globals forming a ring buffer of recently shown images; survives deep sleep, resets on power-off. Already includes `esp_attr.h` so it compiles under esp-idf. Must sit next to `config.yaml` (`esphome.includes`).

**`reference/`** — gitignored context only, never committed: the E1004 schematic PDF (`e1004_schematic.pdf`, official name `202004523_reTerminal E1004_V1.0_SCH_260105.pdf`), the Seeed wiki sources for the E1004 getting-started page and the E-series ESPHome/Arduino cookbooks, and a snapshot of the upstream T133A01 driver from the ESPHome 2026.7.0 tag (`epaper_spi_t133a01.{h,cpp}`, `epaper_spi_model_t133a01.py`).

## Hardware (from schematic v1.0, 2026-01-05, `202004523_reTerminal E1004_V1.0_SCH_260105.pdf` on the Seeed wiki)

### Main ICs

- **MCU**: ESP32-S3R8, 40 MHz crystal, external 256 Mbit (32 MB) SPI flash (ZB25Q256AYJG)
- **Display**: 13.3" Spectra 6 panel on a 60-pin FPC (J2), driven by two T133A01 controller halves (CS + CS1); panel power rails generated by discrete boost/inverter circuitry on the board (no IT8951/TPS651851 equivalent — sequencing handled by the T133A01s)
- **Battery PMIC**: SY6974B (U12) — same part as E1003, I2C 0x6B
- **3V3 DCDC**: TPS631000DRLR (U1) buck-boost, VSYS → SYS_3V3
- **USB-UART**: CH340K (U7) on UART0, powered from USB 5 V via TPL740F33 (only alive when a cable is attached)
- **RTC**: PCF8563M/TR (U11), I2C 0x51, CR1220 backup
- **T&RH sensor**: SHT40-AD1B-R2 (U15), I2C 0x44
- **Buttons**: three front capacitive-touch keys (external touch module on connector J4) AND three back physical keys (TC00104 switches), combined per-key through SN74LVC1G08 AND gates (U16/U20/U21) onto the same three GPIOs — active low, actively driven, no internal pull-ups needed, work during deep sleep (gates run from always-on SYS_3V3)
- **Battery**: Li-ion 5000 mAh

### GPIO pin map (ESP32-S3, net names from schematic)

| GPIO | Signal | Function |
|------|--------|----------|
| 0 | ESP_BOOT | Boot button |
| 1 | VBAT_ADC | Battery voltage ADC (via divider, requires GPIO21 enable) |
| 2 | SPI_CS_S | Display CS1 (second T133A01 half) |
| 3 | KEY0 | Right direction button (front touch + back physical) |
| 4 | KEY1 | Left direction button (front touch + back physical) |
| 5 | KEY2 | Refresh button (front touch + back physical); deep sleep wakeup pin |
| 6 | ADC | Header J3 ADC |
| 7 | SCK | SPI SCK → display + SD card (shared bus) |
| 8 | MISO | SPI MISO (SD card; display is write-only) |
| 9 | MOSI | SPI MOSI → display + SD card (shared bus) |
| 10 | SCREEN_CS# | Display CS (first T133A01 half) |
| 11 | SCREEN_DC# | Display data/command |
| 12 | SCREEN_EN# | Display power enable (TPS22916 U10, 100K pulldown) |
| 13 | SCREEN_BUSY# | Display busy (inverted in model definition) |
| 14 | SD_CS | SD card SPI chip select |
| 15 | SD_DET | SD card detect |
| 16 | SD_EN | SD card power enable (TPS22916 U13, 100K pulldown) |
| 17 | SPI_D2 | Display quad-SPI data 2 (unused in 4-wire mode) |
| 18 | SPI_D3 | Display quad-SPI data 3 (unused in 4-wire mode) |
| 19 | I2C0_SDA | I2C0 SDA — RTC, SHT40, charger PMIC |
| 20 | I2C0_SCL | I2C0 SCL — RTC, SHT40, charger PMIC |
| 21 | VBAT_EN | Battery voltage divider enable (TPS22916 U3, 100K pulldown); Seeed: wait ≥1 ms after enable before reading |
| 38 | SCREEN_RST# | Display reset |
| 39 | I2C1_SDA | Expansion header J3 I2C |
| 40 | I2C1_SCL | Expansion header J3 I2C |
| 41 | TX1 | Expansion header J3 UART |
| 42 | RX1 | Expansion header J3 UART |
| 43 | TX0 | CH340K console |
| 44 | RX0 | CH340K console |
| 45 | BUZZER_EN | Buzzer (MLT-8530 via BC817W Q7, 100K base pulldown), 2700 Hz resonant |
| 46 | POWER_EN | Header 3V3 enable (TPS22916 U14, 100K pulldown) |
| 47 | SY_INT | SY6974B charger /INT (wired on this board, unlike E1003!) |
| 48 | LED | Green user LED (inverted — per Seeed's own Arduino example) |

### I2C devices (I2C0, GPIO19=SDA, GPIO20=SCL)

- **0x44** — SHT40 (read once per wake at `wifi.on_connect`, exposed to HA as diagnostics)
- **0x51** — PCF8563 RTC (same VL-flag/`is_valid()` behavior as documented in the E1003 CLAUDE.md; synced from HA time on `on_time_sync`)
- **0x6B** — SY6974B battery PMIC; REG0A bit 7 (BUS_GD) polled by `check_charger` for USB presence, same as E1003. **Unlike the E1003**, /INT is wired — to GPIO47. GPIO47 is not RTC-capable, so it still cannot wake the chip from deep sleep on charger plug-in; polling remains the mechanism. GPIO47 could give an interrupt-driven charger sensor while awake if 15 s polling ever feels slow (note: /INT is an active-low *pulse* on status change, not a level).

### Display notes (T133A01 / epaper_spi)

- Dual-CS architecture: CS (GPIO10) gets the left 600 pixels of each row, CS1 (GPIO2) the right 600, for all 1600 rows. The driver manages both CS lines itself (`manages_cs = True` in the model) — the `spi:` block must NOT list a cs_pin.
- All display pins default from the model definition; the YAML only needs `spi_id` and `model`.
- Driver auto-sequences: reset → init → transfer → PON → DRF (refresh) → POF → controller deep-sleep (cmd 0x07/0xA5) after every update. Panel and controller are left powered down between updates without any config-side action; GPIO12's pulldown keeps display power off during ESP deep sleep.
- `minimum_update_interval` is 30 s for this model (irrelevant here — `update_interval: never`).

### Power / deep sleep

The E1003's `gpio_hold_en` latch machinery is **deliberately absent**. On the E1003, sleep current was ~5 mA because power-switch enables (SD_EN etc.) floated HIGH in deep sleep. On the E1004 every TPS22916 enable (GPIO12 screen, GPIO16 SD, GPIO21 battery divider, GPIO46 header) and every transistor base (GPIO45 buzzer, GPIO48 LED) has an external ~100 K pulldown, so floating pins during deep sleep turn everything **off** by design. Do not re-add hold logic unless a sleep-current measurement on real hardware shows a problem.

Deep sleep wake is `esp32_ext1_wakeup` ANY_LOW on GPIO5 (refresh button). The button AND gates are powered from always-on SYS_3V3 and actively drive the line high when idle, so no RTC pull-up configuration is needed (the E1003 needed `rtc_gpio_pullup_en`; this board does not).

Untested-on-hardware items (verify when flashing a real device): actual sleep current; real refresh timing (to tune `display_refresh_wait` down from the conservative 60 s); LED polarity (schematic shows an NPN low-side driver which would be non-inverted, but Seeed's own E1004 example code says inverted — the config follows Seeed; flip `inverted:` on `led_output` if the LED behaves backwards).

## Key constraints

- ESPHome `min_version: 2026.7.0` — first release containing the `seeed-reterminal-e1004` epaper_spi model.
- Framework is **esp-idf** (what upstream tests the driver with). No Arduino APIs in lambdas.
- Images must be PNG, pre-dithered to the 6-color Spectra 6 palette, sized exactly 1200×1600 (portrait, `rotation: 0`) or 1600×1200 (landscape, `rotation: 90`), sequentially numbered `1.png`, `2.png`, … with no gaps.
- PSRAM required (`psram: mode: octal`): ~3.8 MB online_image RGB565 buffer + ~960 KB display buffer.
- SPI bus GPIO7/9 is shared with the SD card (CS=GPIO14) — if an SD component is ever added, CS coordination applies; SD power (GPIO16) is off by default.
