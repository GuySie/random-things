# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An ESPHome configuration for using a Seeed reTerminal E1003 (IT8951 e-ink controller, 1872×1404px, 16-level grayscale) as a digital art frame with Home Assistant. The device is an ESP32-S3 running the ESP-IDF framework with PSRAM. It wakes from deep sleep on a configurable schedule, downloads a numbered PNG image from Home Assistant's `/local/` folder, renders it, and sleeps again.

This is a port of the sibling `Seeed-reTerminal-E1003/` repository, which used a fully custom `external_components` driver for the IT8951 controller (written before ESPHome had any support for it, and which required the Arduino framework since its C++ used Arduino-specific APIs directly). This repository instead uses ESPHome's official `it8951` display component (merged in ESPHome 2026.7.0, PR #15346) — no custom C++, no `external_components`. Once the custom driver was gone, nothing else in this config actually needed Arduino either (the last remaining Arduino-specific calls were `digitalRead`/`digitalWrite`/`pinMode` in the deep-sleep GPIO-hold lambda, since replaced with their direct ESP-IDF equivalents — see "Port notes" #5), so this config runs `framework: type: esp-idf` — removing the Arduino compatibility layer from the build entirely, and matching the framework ESPHome's own `it8951` component is actually tested against upstream (its test suite file is named `test.esp32-s3-idf.yaml`).

## Port notes

ESPHome's official `it8951` component ships a built-in `model: seeed-reterminal-e1003` preset whose pin defaults (`busy_pin: GPIO13`, `reset_pin: GPIO12`, `cs_pin: GPIO10`, `enable_pin: [GPIO21, GPIO11]`), `vcom: 1400`, `vcom_register: 0x0002`, `force_temperature: 25`, and `mirror_x: true` match this exact board's schematic and the source repo's CLAUDE.md hardware facts almost perfectly — so the new `display:` block barely needs any manual pin/electrical configuration at all. Two real architectural mismatches were found between the old custom driver and the official component, verified against the actual merged component source (not just docs):

1. **Temperature injection is one-shot at boot, not per-refresh.** The old driver re-sent `SET_TEMP` to the panel before every refresh, using a fresh SHT40 reading each time. The official component only ever sends `SET_TEMP` once, during its own internal async boot-time init (`Phase::INIT_TEMP`, part of `INIT_RESET → INIT_DEV_INFO → INIT_VCOM → INIT_TEMP → INIT_DONE`, kicked off from `setup()` at `PROCESSOR` priority = 400 but driven by `loop()`). There is no public re-send/reset API — `set_force_temperature()` after that phase has passed has no further effect until the next full reboot (`recover_()`, which could re-run it, is private and only fires automatically on an unresponsive controller). `force_temperature_` is never *written* by the init sequence itself, only *read*, exactly once, at the `INIT_VCOM → INIT_TEMP` transition — so there's no risk of it8951 "overriding" a value we've set; the only risk is our value arriving too late to be there when that one read happens.

   The first version of this override used `component.update: sht40_sensor` from a high-priority `on_boot` handler, racing it against it8951's setup — but that's genuinely a race, not a guarantee: `SHT4XComponent::update()` schedules its result via a `set_timeout(10, ...)` **deferred** callback, which only fires once `loop()` starts running, which is *after* every component's `setup()` (including it8951's) has already been called. A plain `delay()` in `on_boot` doesn't fix this either — it blocks the whole boot sequence uniformly, pushing back both it8951's start time *and* the point where deferred callbacks get serviced, so it doesn't close the relative gap. And there's no YAML-exposed way to change it8951's own hardcoded `PROCESSOR` (400) setup priority to push it later.

   **The fix used here**: skip the `sht4x` component's async path entirely for this purpose. The `on_boot: priority: 550` handler instead does a **raw synchronous I2C read** of the SHT40 (address `0x44`), matching the ESPHome `sht4x` component's own wire protocol exactly (`0xFD` high-precision measurement command, 10ms conversion delay, then reading back the 6-byte result and converting via the standard SHT4x datasheet formula `T = -45 + 175 × (raw / 65535)`) — the same raw-I2C pattern this config already uses for the `check_charger` script. Because this read is fully synchronous (blocks and returns within the lambda, no scheduler involved) and this handler's priority (550) runs strictly *before* it8951's `setup()` (400) in the same synchronous, priority-ordered boot pass, the whole read-and-override sequence is **guaranteed** to complete before it8951 even starts its init chain. This isn't a race with favorable odds — there's no window left for it8951 to get there first. The `sht4x` sensor component itself is kept in `config.yaml` only for potential future use (`internal: true`, doing nothing right now) — see the comment there.

   **Verified working on real hardware (2026-08-09):** `it8951`'s `dump_config()` output on first flash showed `Force temperature: 26 °C` — not the model's `25 °C` default, and a plausible real-ambient reading — confirming the raw I2C override lands before it8951's `setup()`/`dump_config()` runs, exactly as designed. `dump_config()`'s printed "Force temperature" line **is** a reliable way to confirm this override on this design (unlike the old racy async version, where it could only ever show the stale default): since our override is now synchronous and guaranteed to complete during the `on_boot: priority: 550` step of `App.setup()`, strictly before it8951's own `setup()` call, whatever `dump_config()` prints later already reflects it.

   The on_boot lambda also logs `ESP_LOGD("temp_override", "set_force_temperature(%d) [%.2f C, sync I2C] at %u ms", ...)` right after the raw read — but don't expect to see this over OTA/API logs (`esphome logs`): it fires essentially at the very start of `setup()`, long before wifi associates, while the OTA log viewer can only start receiving lines *after* it establishes a live API connection (which itself takes several seconds, since it needs wifi + mDNS + an API handshake first). By the time a remote log viewer attaches, this line has already been emitted and is gone — this is a structural limitation of API-based log streaming for anything pre-wifi, not a bug. Seeing it directly would require a serial/UART console attached from power-on. `dump_config()`'s line is the practical way to confirm this on real hardware, as demonstrated above.

   For convenience, the value is also re-logged later, well after wifi/API are up: the `globals:` entry `last_temp_override` (a plain `float`, `restore_value: false`) stashes whatever the on_boot lambda computed, and `online_image`'s `on_download_finished:` handler logs it again (`"Temperature used for this boot's it8951 init: %.2f C"`) right next to the existing `img_history` log line — which by the real trace above fires tens of seconds into the awake session, comfortably after any OTA log viewer would have connected regardless of exact timing. This is purely a convenience re-log for visibility; it doesn't affect the override itself, which has already happened by then.

   For deeper investigation if this ever needs re-diagnosing (e.g. after an ESPHome update changes internals): `Phase` is a plain `enum class Phase : uint8_t { IDLE, INIT_RESET, INIT_DEV_INFO, INIT_VCOM, INIT_TEMP, INIT_DONE, UPDATE_PREPARE, UPDATE_TRANSFER, UPDATE_REFRESH, UPDATE_SLEEP }`, so its members number 0–9 in that order — `INIT_VCOM` = 3, `INIT_TEMP` = 4. `logger: logs: {it8951: VERBOSE}` shows its `Phase %u -> %u` transition log; the line to watch for is `Phase 3 -> 4`, the exact instant `enqueue_init_temp_()` reads `force_temperature_` and queues it for the panel.

   **Empirical visual test** (conclusive, independent of any of the above): temporarily change the lambda to call `set_force_temperature()` with a hardcoded extreme value (e.g. -40 or 85, the ends of its valid range) instead of the real SHT40 reading, and compare the rendered grayscale output against a normal boot. A dramatic difference in waveform/grayscale rendering confirms the override is reaching the panel.

   **If this still somehow doesn't work on real hardware** (e.g. if it8951's actual `PROCESSOR` priority turns out to differ from 400 in a future ESPHome version, or some other component construction order assumption above is wrong): drop the `set_force_temperature` call, accept the model's fixed `force_temperature: 25` default, and remove the now-pointless `sht4x` sensor block.

2. **Update-action calls coalesce; a literal INIT-then-GC16 double refresh isn't achievable.** The old driver always did a full INIT (clear) pass immediately followed by a GC16 pass on every wake, to eliminate ghosting since the whole image changes every cycle. The official component's `it8951.update` action queue coalesces: a second call arriving before the first reaches `Phase::IDLE` just overwrites the pending mode (`start_update_()`: "Coalesce: latest pending mode wins") rather than queuing an independent pass, and even with a delay between calls, a second call with no framebuffer change in between finds an empty dirty region and silently no-ops (`prepare_update_region_()` returns false, "nothing to update"). Two sequential `it8951.update` actions therefore do not produce two real hardware passes.

   The fix used here: `full_update_every: 1` + a single `it8951.update: {mode: GC16}` per wake. `prepare_update_region_()`'s "full update" branch unconditionally forces the whole-panel region and `UPDATE_MODE_GC16`, bypassing dirty-region tracking entirely — this deterministically matches the old driver's "always full, never partial" intent, just without a separate preceding clear pass.

   **Verification**: with VERBOSE logging, confirm exactly one full-panel GC16 pass happens per wake and isn't silently skipped. Run several consecutive wake/refresh cycles with visually dissimilar images and inspect for ghosting versus the old device.

3. **Rotation/mirror mechanism is architecturally different.** The old driver achieved portrait mode by combining ESPHome's standard `rotation:` transform with an *unconditional* x-mirror in its own `draw_absolute_pixel_internal` (compensating for the ED103TC2 panel's native top-right-origin scan direction, independent of the user's rotation choice). The official component does the equivalent via `rotation:` composed with a `transform: {mirror_x: true}` (the model default) as an ESP32-side coordinate transform (`apply_transform_`) — same intent, different code path. `rotation: ${rotation}` (0/270) carries over unchanged, but identical numeric values are not guaranteed to produce identical visual output — **verify on-device with an intentionally asymmetric test image** (e.g. a letter or arrow near one corner, not a symmetric photo) for both `rotation: 0` and `rotation: 270`.

4. **GPIO11/GPIO21 are now also owned by `enable_pin`.** The existing `on_shutdown` (priority 50) handler drives GPIO11 and GPIO21 LOW via raw `gpio_set_direction()`/`gpio_set_level()`/`gpio_hold_en()` — the same two pins the it8951 component now manages via `enable_pin: [GPIO21, GPIO11]`. This needed no changes: ESPHome's pin-conflict validation only tracks pins declared through YAML `pin:` schemas, not raw GPIO numbers used inside a `lambda:`, so there's no compile conflict (same as with the old custom driver, which also hardcoded these pins in C++ rather than declaring them via YAML). Sequencing still holds too — the priority-900 `on_boot` handler (`gpio_hold_dis`, releasing the previous sleep cycle's latch) still runs before it8951's `setup()` (900 > 400, same ordering guarantee as before, actually more margin than the old driver's `HARDWARE` = 800). The one real (harmless) behavioral difference: the old driver explicitly dropped GPIO11 LOW after every refresh as a mid-cycle optimization; the official component's `sleep_when_done: false` default (this model's default) leaves both pins HIGH after refresh until something else changes them. On this device that only extends the HIGH duration by a few seconds during the ~90s awake window before the unchanged `on_shutdown` handler unconditionally cuts both pins — negligible against the documented ~185mA awake budget.

5. **Framework switched from Arduino to ESP-IDF.** The source repo's custom driver needed the Arduino framework because its C++ used Arduino-specific APIs (`digitalWrite`, `pinMode`, `SPI.transfer16`, etc.) directly. Once that driver was gone, the only remaining Arduino-specific calls in this config were `digitalRead`/`digitalWrite`/`pinMode`/`OUTPUT` in the `on_shutdown` priority-50 GPIO-hold lambda — rewritten to their direct ESP-IDF equivalents (`gpio_get_level`/`gpio_set_level`/`gpio_set_direction`/`GPIO_MODE_OUTPUT`), which the same lambda already used side-by-side for other pins (e.g. `gpio_set_direction(GPIO_NUM_19, GPIO_MODE_DISABLE)`), so this was a like-for-like substitution, not new logic. `delay()`/`millis()` (used in the temperature-override lambda) didn't need changing — they're provided by ESPHome's own ESP32 HAL (`esphome/components/esp32/hal.h`) regardless of framework, guarded only by `USE_ESP32`, not `USE_ARDUINO`.

   **Why bother**: removes the Arduino compatibility layer from the build entirely (current ESPHome already builds Arduino-on-ESP32-S3 as a component *within* the full ESP-IDF build system, so this doesn't avoid an ESP-IDF build, just the extra Arduino core on top of it), and matches the framework `it8951` is actually tested against upstream (its test suite file is `test.esp32-s3-idf.yaml`).

   **Checked for hidden differences before switching**: the actual GPIO-hold/deep-sleep mechanism (`gpio_hold_en`, `gpio_deep_sleep_hold_en`, `esp_sleep_pd_config`, `rtc_gpio_pullup_en`/`rtc_gpio_pulldown_dis`) was *already* pure ESP-IDF code even under the Arduino-framework build — only the pin-set calls feeding into it had Arduino syntax sugar over the identical underlying `gpio` driver operations, so no behavior change is expected there. Also checked ESPHome's `esp32` component for framework-conditional sdkconfig differences that could plausibly affect deep-sleep quiescent current (`CONFIG_PM_*`, tickless idle, CPU frequency scaling, watchdog defaults) — found none. The one confirmed framework-conditional difference (`CONFIG_ESP_WIFI_SOFTAP_SUPPORT`/DHCP-server Kconfig, disabled under ESP-IDF when no AP is configured) doesn't apply here since this config already has `wifi: ap:` configured, and wouldn't affect deep-sleep current even where it does apply (it's about awake-time WiFi stack footprint; `esp_deep_sleep_start()` powers down the RF domain regardless of what was compiled in).

   **Still worth re-verifying on real hardware, not just trusting this reasoning**: re-run the sleep-current check (BM235-style meter, `[power]` BEFORE/AFTER log lines) after this switch, same as after the original port — this config's sleep-current tuning came from real measurement with some genuinely non-obvious findings (see the note below), so a framework change is exactly the kind of thing worth re-confirming empirically rather than assuming from source-level reasoning alone.

Everything else (wifi, deep sleep/RTC scheduling, battery, buttons, buzzer, charger detection, `online_image`/`image: platform: online_image` download flow, all HA-exposed entities, `image_history.h` ring buffer) is unchanged from the source repo.

## ESPHome references

- **Source code**: https://github.com/esphome/esphome — component implementations live under `esphome/components/`. Python codegen is in `__init__.py` or a named `.py` file; C++ runtime in `.h`/`.cpp` alongside it. The `it8951` component lives at `esphome/components/it8951/` — see `display.py` for the YAML schema/model presets and `it8951.cpp`/`it8951.h` for the async init/update state machine referenced above.
- **YAML docs**: https://esphome.io — authoritative reference for component options, action/condition syntax, and what's available without reading C++. `it8951` docs: https://esphome.io/components/display/it8951/
- **ESP-IDF API docs**: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/ — reference for C++ APIs used directly in lambdas (`RTC_DATA_ATTR`, `esp_random()`, `MALLOC_CAP_SPIRAM`, deep sleep, `gpio_*`). This config uses the ESP-IDF framework directly (see "Port notes" #5), so these are the primary, authoritative docs — not a secondary reference behind an Arduino wrapper.

## Reading PDFs in this repo

Use the Read tool **without** specifying a `pages` parameter — calling it on a PDF without pages reads the whole document successfully. Specifying `pages` requires `pdftoppm` (poppler) which is not installed and will error.

## Architecture

**`config.yaml`** — the entire device logic lives here. Substitutions are at the top and set compile-time defaults; several settings can also be changed at runtime from Home Assistant without reflashing (see below). Key flows:

- `wifi.fast_connect: true` is set so the device skips active scanning and connects faster on known networks — important for minimising active wake time.
- `wifi.on_connect` triggers `script.execute: check_charger` (see charger-detection note below), `component.update: sht40_sensor` (now vestigial for display purposes — see Port notes #1 — but kept for the HA-exposed reading and for wifi-reconnect refresh), `component.update: battery_voltage` (so battery level is read once per wake cycle while wifi is active), and then `random_image` button, which picks an image (mode controlled by the `use_image_history` switch — see below), sets `image_counter`, which triggers `online_image.set_url`, which triggers a download. On download completion, `it8951.update: {id: epaper_display, mode: GC16}` is triggered, the image number is recorded to the history buffer, and then `deep_sleep.enter` is called directly (gated — see below). On download error, `deep_sleep.enter` is also called directly under the same gate. This means the device enters deep sleep from the `image`/`online_image` callbacks rather than waiting for any external HA trigger.
- `on_shutdown` has three handlers: priority 200 disables the battery voltage divider output (`bsp_battery_enable`) before any shutdown path; priority 100 computes the clock-aligned sleep duration from the PCF8563 RTC — it finds the next boundary at `rtc_interval_hours`-hour intervals, advances past any boundaries outside `rtc_hour_start`–`rtc_hour_end`, and falls back to one full interval if the RTC is not set. Computing at shutdown (rather than boot) accounts for actual time spent awake and gives more accurate wake-up alignment. Priority 50 runs the GPIO hold sequence (see power optimisation note below, and Port notes #4).
- Deep sleep is gated by a Home Assistant `input_boolean` synced via the `allow_deepsleep` homeassistant switch, AND by USB charger presence. The refresh schedule (`Refresh Interval`, `Active Hours Start`, `Active Hours End`) is configurable from HA without reflashing via persistent `number` entities (defaults: 1 h interval, 07:00–23:00 window). The green button (GPIO3) wakes the device via `esp32_ext1_wakeup` (ANY_LOW) — ext1 is required because `gpio_deep_sleep_hold_en()` breaks ext0 wakeup on the S3.
- **USB charger detection**: the `check_charger` script reads register 0x0A bit 7 (BUS_GD) from the SY6974B PMIC over I2C0 (address 0x6B) and publishes the result to the `charger_connected` binary_sensor (`device_class: plug`); BUS_GD is set whenever a cable is physically attached to VBUS, ahead of any input-source qualification. The script runs once at `wifi.on_connect` (so state is known before the download completes) and every 15 s via an `interval:` component while awake (so unplugging is noticed without waiting for the next scheduled wake). `charger_connected`'s `on_press`/`on_release` call `deep_sleep.prevent`/`deep_sleep.allow` on `deep_sleep_1` — `allow` only fires if `allow_deepsleep` is also true, so an HA-side disable still wins. This only affects the automatic run_duration-timeout sleep path: ESPHome's `deep_sleep.enter` action calls `begin_sleep(true)`, which bypasses the `prevent_` flag entirely (see `deep_sleep_component.cpp`), so the two `deep_sleep.enter` calls in the `image`/`online_image` callbacks are separately gated with `&& !id(charger_connected).state` rather than relying on prevent/allow alone. Net effect: while a charger is connected, the device stays awake indefinitely instead of cycling through its normal sleep schedule.
- The image subfolder is set via a persistent `text` entity (`album_folder_text`, "Album Folder") editable from HA at runtime. The initial value is `""` (images served from the www root). No trailing slash is needed — the URL lambda appends one automatically when the folder is non-empty.
- The PCF8563 RTC (`update_interval: never`) is synced from the `homeassistant` time platform on each `on_time_sync` event, so the clock stays accurate across deep sleep cycles.
- Battery voltage is measured through a voltage divider on GPIO1, enabled via GPIO40. Battery level is derived via `calibrate_linear` filter.
- `on_boot` has three handlers: priority 900 calls `gpio_hold_dis` on all held pins (GPIO11, 21, 16, 38, 39, 45, 48) before the it8951 component's `PROCESSOR`-priority (400) `setup()` runs and drives `enable_pin` HIGH. Priority 600 enables the battery voltage divider and triggers `component.update: last_boot_sensor`. Priority 550 (new — see Port notes #1) kicks off the SHT40 read to race the it8951 component's temperature init. The `last_boot_sensor` is a `text_sensor` (platform: template, `device_class: timestamp`, `update_interval: never`) that reads `id(rtc_time).now()` and formats it as a UTC ISO 8601 string (`%Y-%m-%dT%H:%M:%S+00:00`). Because the PCF8563 is coin-cell-backed and set to UTC, the boot timestamp is available immediately without waiting for HA time sync. Returns `{}` (unavailable) if the RTC VL flag is set (first boot before any time sync).

**Deep sleep power optimisation** — without GPIO holds, measured sleep current is ~5 mA (draining ~3%/day). The root causes are GPIO39 (SD_EN, U13 TPS22916 load switch) and GPIO48 (TOUCH_RES, GT911 touch controller reset, active-low) floating HIGH during sleep because a bare `gpio_set_level()` write is silently discarded at sleep entry on the ESP32-S3 unless explicitly latched. The fix: `on_shutdown` priority 50 calls `gpio_hold_en()` on GPIO11, 21, 38, 39, 45, 48 (GPIO16/USER_LED excluded — latching it LOW drives the LED on via Q4 transistor, causing stuck-on LED during sleep), then `gpio_deep_sleep_hold_en()` to arm the hold domain for non-RTC pins (>GPIO21). `<driver/rtc_io.h>` is included (via `esphome.includes`) for `rtc_gpio_pullup_en`/`rtc_gpio_pulldown_dis` on the wakeup pin (GPIO3) — this is a native ESP-IDF header, resolved from the downloaded ESP-IDF framework tree (`.esphome/idf/frameworks/<version>/components/driver/.../include/driver/rtc_io.h`) now that this config uses `framework: type: esp-idf` (see "Port notes" #5); it previously resolved from the Arduino core's vendored copy under the Arduino-framework build, but it's the same underlying ESP-IDF header either way. With holds applied, sleep current drops to sub-milliamp (below BM235 meter resolution) on the source repo's custom-driver, Arduino-framework build — **re-verify this is still true** on this port, both for the it8951 component swap (see Port notes #4) and for the ESP-IDF framework switch (see Port notes #5) — neither is expected to change it based on source-level analysis, but this figure hasn't yet been re-measured on real hardware after either change. Verified (on the pre-ESP-IDF-switch build): the `[power]` BEFORE/AFTER log lines at shutdown show IO39 and IO48 transitioning from 1 to 0. Awake current is ~185 mA; a full wake cycle (connect, download, render, sleep) takes ~90 s, so awake energy is a small fraction of the daily budget.

Fixes confirmed to have **no effect** on sleep current (do not re-investigate these): bare `digitalWrite()` LOW on GPIO21 or GPIO11 before sleep (silently discarded by ESP32-S3 at sleep entry); switching to ext1 wakeup alone without holds; holding GPIO21/GPIO11 only (IT8951 display rail is negligible at idle — zero change from 4.95 mA baseline in testing); GPIO46 (Header_EN) is already LOW by default.

The irreducible hardware quiescent floor is ~90 µA: SY6974B charger ~75 µA + TPS631000DRLR buck-boost ~15 µA + PCF8563 RTC ~250 nA. These are always-on and not firmware-addressable.

Checked and ruled out (do not re-investigate): ESP-IDF sdkconfig flags `CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND`, `CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND`, and `CONFIG_ESP_SLEEP_POWER_DOWN_FLASH` only affect `esp_light_sleep_start()` (per ESP-IDF's `esp_hw_support/Kconfig`); this firmware never enters automatic light sleep — `deep_sleep.enter` goes straight to `esp_deep_sleep_start()`, which already powers down the flash/PSRAM domain unconditionally as part of full deep sleep. The first two also already default to `y` out of the box, and `POWER_DOWN_FLASH` additionally requires no-SPIRAM-or-dedicated-PSRAM-LDO (`depends on !SPIRAM || ESP_LDO_RESERVE_PSRAM || SOC_PSRAM_HAS_DEDICATED_LDO`), which this board's shared-rail octal PSRAM doesn't satisfy. `CONFIG_ESP_SLEEP_RTC_BUS_ISO_WORKAROUND` has no user prompt and already defaults to `y` for `IDF_TARGET_ESP32S3`. None of these are levers available to us.

**`image_history.h`** — declares three `RTC_DATA_ATTR` globals (`g_history[50]`, `g_hist_head`, `g_hist_count`) forming a circular ring buffer of recently displayed image numbers. `RTC_DATA_ATTR` means the buffer survives deep sleep but resets on full power-off. Must be in the same directory as `config.yaml`; included via `esphome.includes`. The `random_image` button has two modes controlled by the `use_image_history` switch (HA entity, defaults ON): when ON it avoids images in the last `floor(total/2)` entries (capped at 50, always leaving at least one free slot), falling back to the oldest entry in the window after 200 failed attempts; when OFF it picks any image from 1..total with no restriction. The history buffer is always recorded on download regardless of mode, so switching back to history mode immediately benefits from prior context.

**The `it8951` display component** is entirely upstream ESPHome now — see the Port notes section above for how it's configured and where it differs from the old custom driver. For low-level IT8951 protocol/register detail (SPI command set, LD_IMG alignment rules, display mode numbers, power-sequencing internals) that's no longer this repo's concern to maintain, see the sibling `Seeed-reTerminal-E1003/CLAUDE.md` (which documents it against the old custom driver) or the official component's own source.

## Hardware (from schematic v1.0, 2025-12-25)

### Main ICs
- **MCU**: ESP32-S3R8 with 8MB PSRAM, 40MHz crystal, external 256Mbit (32MB) SPI flash (ZB25Q256AYJG)
- **E-ink controller**: IT8951E/DX (U23), 12MHz crystal, 32Mbit SPI flash (ZB25VQ32DSJG) for waveforms
- **E-ink panel**: ED103TC2 (10.3-inch), connected via 40-pin FPC (AFC24-S40FIA-00, J8); FPC pin 1 → screen pin 6
- **EPD PMIC**: TPS651851RSLR (U21) — generates display supply rails (VDDH ~+28V, VPOS ~+15V, VEE ~−20V, VCOM); VCOM set via I2C from IT8951
- **Battery PMIC**: SY6974B (U2) — Li-ion charge controller, USB-C input (TYPEC_5V), NTC thermistor monitoring, I2C status reporting
- **3V3 DCDC**: TPS631000DRLR (U1) — bidirectional buck-boost, VSYS → SYS_3V3 (1.5A max, PFM/PWM selectable)
- **1V8 DCDC**: ETA3410D2I-T — VSYS → VCC_ITE_1V8 (2A max, powers IT8951 core and analog)
- **USB-UART**: CH340K (U8), enabled by CH340_EN; 40MHz crystal shared path
- **RTC**: PCF8563M/TR (U11), I2C address 0x51, backed by CR1220 coin cell (BAT1), 32.768kHz crystal (FC-12M)
- **T&RH sensor**: SHT40-AD1B-R2 (U15), I2C address 0x44; layout: keep away from heat, mill groove, no copper under sensor
- **PDM microphones**: dual MSM261DHP006 (U17, U18), stereo pair (one L, one R)
- **Battery**: Li-ion 3000mAh

### GPIO pin map (ESP32-S3)
| GPIO | Signal | Function |
|------|--------|----------|
| 0 | ESP_BOOT | Boot button |
| 1 | VBAT_ADC | Battery voltage ADC input (via divider, requires GPIO40 enable) |
| 2 | TOUCH_INT | Touch panel interrupt |
| 3 | KEY0 | Button 0; deep sleep wakeup pin |
| 4 | KEY1 | Button 1 |
| 5 | KEY2 | Button 2 |
| 6 | ADC1_CH | Header ADC channel |
| 7 | ITE_SD_SCK | SPI SCK → IT8951 and SD card (shared bus) |
| 8 | ITE_SD_MISO | SPI MISO ← IT8951 and SD card (shared bus) |
| 9 | ITE_SD_MOSI | SPI MOSI → IT8951 and SD card (shared bus) |
| 10 | ITE_CS# | IT8951 SPI chip select |
| 11 | EPD_Drive_EN | Enable VSYS → EPD_Drive power switch (U22); gates TPS651851RSLR supply |
| 12 | ITE_RST# | IT8951 hardware reset (active low) |
| 13 | ITE_BUSY# | IT8951 HRDY / busy (active low) |
| 14 | SD_CS | SD card SPI chip select |
| 15 | SD_DET | SD card detect switch |
| 16 | USER_LED | Green user LED (D7) |
| 17 | TX1 | UART1 TX (IT8951 ITE_RX for debug) |
| 18 | RX1 | UART1 RX (IT8951 ITE_TX for debug) |
| 19 | I2C0_SDA | I2C0 SDA — PMIC, RTC, SHT40, touch panel |
| 20 | I2C0_SCL | I2C0 SCL — PMIC, RTC, SHT40, touch panel |
| 21 | ITE_VCC_EN | Enable 1V8 power switch to IT8951 (U20) |
| 38 | PDM_EN | PDM microphone power enable |
| 39 | SD_EN | SD card power enable (U13) |
| 40 | VBAT_EN | Enable battery voltage divider (U3) for ADC reading |
| 41 | PDM_DATA | PDM microphone data |
| 42 | PDM_CLK | PDM microphone clock |
| 45 | BUZZER_EN | Buzzer enable (via BC817W transistor Q7) |
| 46 | Header_EN | Enable 3V3 on header J2 (U28) |
| 47 | — | Header J2 GPIO |
| 48 | TOUCH_RES | Touch panel reset |

### I2C devices (I2C0, GPIO19=SDA, GPIO20=SCL)
- **0x44** — SHT40 temperature sensor (`update_interval: never`; triggered as early as possible in `on_boot` at priority 550 — see Port notes #1 — and again at `wifi.on_connect`; `internal: true` — value is forwarded to IT8951 only, not exposed to HA)
- **0x51** — PCF8563 RTC
- **0x5D** — GT911 touch controller (TOUCH_RES=GPIO48 active-low, TOUCH_INT=GPIO2; not used in art-frame config)
- **0x6B** — SY6974B battery PMIC; register 0x0A bit 7 (BUS_GD) is read by the `check_charger` script for USB charger detection (see deep sleep gating note above). Full register map: `reference/DS_SY6974B.pdf` in the sibling source repo. Its `/INT` pin (pin 7, active-low, pulses on charger status changes) is **not wired to the ESP32** — the schematic's `03 Power` sheet gives it a net label (`ESP_IO/SY_INT`) with no GPIO number, and no matching label exists anywhere on the `04 ESP32-S3` pin-out sheet, so the trace only reaches test point TP53. This rules out a true hardware wake-from-deep-sleep on charger plug-in; charger presence can only be polled after boot (which is what `check_charger` does). Do not re-investigate this without new schematic evidence.
- TPS651851RSLR EPD PMIC is controlled by IT8951 over a separate internal I2C path (not directly from ESP32)

### SPI bus sharing
GPIO7/8/9 (SCK/MISO/MOSI) are shared between the IT8951 (CS=GPIO10) and the SD card (CS=GPIO14). Both must never be chip-selected simultaneously. No SD component is configured in this config, so only the it8951 component drives the bus.

### Power architecture
- USB-C → SY6974B PMIC → VSYS (battery-backed system rail)
- VSYS → TPS631000DRLR → SYS_3V3 (ESP32, peripherals)
- VSYS → ETA3410D2I-T → VCC_ITE_1V8 (IT8951 core/analog)
- VSYS → U22 (TPS22916, gated by GPIO11/EPD_Drive_EN) → EPD_Drive → TPS651851RSLR input
- SYS_3V3 → U20 (TPS22916, gated by GPIO21/ITE_VCC_EN) → VCC_ITE_1V8 to IT8951
- IT8951 controls TPS651851RSLR over I2C for VCOM and display rail sequencing (PWR_WAKEUP, PWRUP, PWRCOM, PWR_GOOD signals routed through IT8951 GPIO8–GPIO13)
- Battery voltage: GPIO40 enables U3 (voltage divider power switch); GPIO1 reads the divided voltage

### IT8951 interface mode
The IT8951E/DX uses the SPI host interface (not I80 parallel). HOST_HDB0–15 parallel pins are not connected to the ESP32. HRDY (HOST_HRDY, pin 13) is the busy signal → GPIO13.

### Peripherals present on board
- 5 buttons: BOOT (GPIO0), KEY0 (GPIO3, wakeup), KEY1 (GPIO4), KEY2 (GPIO5), RST (hardware)
- Green user LED: GPIO16; LOW drives the LED on via Q4 transistor (inverted logic) — do not hold GPIO16 LOW during sleep or the LED stays on
- Buzzer: MLT-8530 on GPIO45; driven at 2700 Hz (resonant frequency) with 20 ms pulses for a crisp tactile click
- Dual PDM stereo microphone: GPIO38 (enable), GPIO41 (data), GPIO42 (clock)
- MicroSD slot: SPI on GPIO7/8/9/14, detect GPIO15, power GPIO39
- Touch panel FPC (J3, 8-pin): I2C0, INT=GPIO2, RST=GPIO48 (not used in art-frame config)
- 6-pin 2.54mm header (J2): includes HEADER_3V3 (GPIO46), GPIO47, GPIO6/ADC, I2C0

## Key constraints

- `vcom` (set via the `seeed-reterminal-e1003` model default, 1400 = 1.400 V) must match the panel's physical VCOM voltage. Wrong VCOM causes poor contrast or display damage over time. If a replacement panel with a different label value is ever fitted, override `vcom:` explicitly in the `display:` block.
- Images must be PNG, 16-level grayscale, and sized exactly to the rotation: 1404×1872 (portrait/270°) or 1872×1404 (landscape/0°). Files must be sequentially numbered `1.png`, `2.png`, … with no gaps.
- ESPHome `min_version: 2026.7.0` is required (first version containing the `it8951` component).
- PSRAM must be present and enabled (`psram: mode: octal`) — the display component allocates its framebuffer there.
- `spi:` (`clk_pin`/`mosi_pin`/`miso_pin`) must be declared at the top level — `miso_pin` is mandatory for `it8951` (it reads DevInfo/VCOM/LUT-busy state back over MISO, unlike typical write-only e-paper SPI drivers); omitting it fails config validation.

## ED103TC2 panel reference

- **Physical**: active area 157.25 × 209.66 mm, outline 174.4 × 216.7 × 0.78 mm, 60 ±7 g, 112 µm square pixel pitch, 1404(V)×1872(H) landscape-native resolution, 2–16 grayscale levels depending on waveform.
- **Native scan direction**: origin at the panel's top-right corner in landscape — this is why `mirror_x: true` is the correct default for this board (see Port notes #3).
- **Temperature limits**: operating 0–50°C, storage −25–70°C.
- **VCOM tolerance**: ±0.1V from the value printed on the panel label; inrush current up to 0.8A at turn-on.
- **Power-on/off sequencing constraints** (source chain VSS→VDD→VNEG→VPOS→VCOM; gate chain VSS→VDD→VGL→VGH; VGL must stay negative of VCOM throughout discharge; ≥500ms total discharge time) are handled automatically by the IT8951's POWER_SEQ auto-management — not something this config or the official component needs to manage directly.
- **Power consumption**: active refresh 380mW–5.5W typical→max; standby (image held, controller idle) ≤1.22mW — E-ink is bi-stable, no power needed to hold an image.

Full panel spec detail (FPC pinout, optical characteristics, handling notes) is in the sibling `Seeed-reTerminal-E1003/CLAUDE.md` and `reference/Eink P-511-828-V1_ED103TC2 Formal Spec V1.0_20190514.pdf`.

## PCF8563 RTC protocol reference

Source: NXP PCF8563 product data sheet Rev. 11.1 (19 January 2026).

I2C address 0x51, max 400kHz, BCD-encoded time fields, register auto-increment allows a single-burst read of seconds through years (0x02–0x08). The VL (Voltage-Low) flag at bit 7 of register 0x02 defaults to 1 on power-up/coin-cell-dead and makes ESPHome's `now().is_valid()` return false until `pcf8563.write_time` clears it (triggered here by `homeassistant.on_time_sync`) — this is why the `on_shutdown` lambda falls back to a fixed one-interval sleep when `!t.is_valid()`. Century flag (bit 7 of register 0x07) toggles automatically on year rollover.

Full register map and setting/reading procedure is in the sibling `Seeed-reTerminal-E1003/CLAUDE.md`.
