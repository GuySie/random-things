# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An ESPHome configuration for using a Seeed reTerminal E1003 (IT8951 e-ink controller, 1872×1404px, 16-level grayscale) as a digital art frame with Home Assistant. The device is an ESP32-S3 running Arduino framework with PSRAM. It wakes from deep sleep on a configurable schedule, downloads a numbered PNG image from Home Assistant's `/local/` folder, renders it, and sleeps again.

## ESPHome references

- **Source code**: https://github.com/esphome/esphome — component implementations live under `esphome/components/`. Python codegen is in `__init__.py` or a named `.py` file; C++ runtime in `.h`/`.cpp` alongside it.
- **YAML docs**: https://esphome.io — authoritative reference for component options, action/condition syntax, and what's available without reading C++.
- **ESP-IDF API docs**: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/ — reference for C++ APIs used directly in lambdas (`RTC_DATA_ATTR`, `esp_random()`, `MALLOC_CAP_SPIRAM`, deep sleep). The Arduino framework wraps ESP-IDF so IDF docs are the authoritative source for those calls.

## Reading PDFs in this repo

Use the Read tool **without** specifying a `pages` parameter — calling it on a PDF without pages reads the whole document successfully. Specifying `pages` requires `pdftoppm` (poppler) which is not installed and will error.

## Components

The `components/` folder must be in the same directory as `config.yaml` — ESPHome resolves the `local` source path relative to the config file.

## Architecture

**`config.yaml`** — the entire device logic lives here. All substitutions are at the top and are the primary configuration surface for end users. Key flows:

- `wifi.on_connect` triggers `random_image` button, which picks an image (mode controlled by the `use_image_history` switch — see below), sets `image_counter`, which triggers `online_image.set_url`, which triggers a download, which on completion triggers `component.update: epaper_display` and records the image number to the history buffer.
- Deep sleep duration is computed dynamically just before sleep (priority 100 `on_shutdown` lambda) using the PCF8563 RTC: it finds the next clock-aligned boundary at `rtc_interval_hours`-hour intervals, then advances past any boundaries outside the `rtc_hour_start`–`rtc_hour_end` active window. Falls back to sleeping one full interval if the RTC is not set. Computing at shutdown (rather than boot) accounts for actual time spent awake and gives more accurate wake-up alignment.
- Deep sleep is gated by a Home Assistant `input_boolean` synced via the `allow_deepsleep` homeassistant switch. The refresh schedule (`Refresh Interval`, `Active Hours Start`, `Active Hours End`) is configurable from HA without reflashing via persistent `number` entities (defaults: 1 h interval, 07:00–23:00 window). The green button (GPIO3) is wired as a deep sleep wakeup pin.
- The PCF8563 RTC (`update_interval: never`) is synced from the `homeassistant` time platform on each `on_time_sync` event, so the clock stays accurate across deep sleep cycles.
- Battery voltage is measured through a voltage divider on GPIO1, enabled via GPIO40. Battery level is derived via `calibrate_linear` filter.

**`image_history.h`** — declares three `RTC_DATA_ATTR` globals (`g_history[50]`, `g_hist_head`, `g_hist_count`) forming a circular ring buffer of recently displayed image numbers. `RTC_DATA_ATTR` means the buffer survives deep sleep but resets on full power-off. Must be in the same directory as `config.yaml`; included via `esphome.includes`. The `random_image` button has two modes controlled by the `use_image_history` switch (HA entity, defaults ON): when ON it avoids images in the last `floor(total/2)` entries (capped at 50, always leaving at least one free slot), falling back to the oldest entry in the window after 200 failed attempts; when OFF it picks any image from 1..total with no restriction. The history buffer is always recorded on download regardless of mode, so switching back to history mode immediately benefits from prior context.

**`components/it8951_reterminal_e1003/`** — a custom ESPHome display component for the IT8951 controller (not in upstream ESPHome):

- `display.py` — ESPHome Python codegen; registers the component and exposes `vcom` config option.
- `it8951_reterminal_e1003.h/.cpp` — C++ driver. SPI pins are hardcoded to the E1003 schematic (SCK=7, MISO=8, MOSI=9, CS=10, BUSY=13, EN=11, RST=12, ITE_EN=21). On `setup()`, it probes the controller through up to 4 power-cycle attempts to handle cold-boot and wake-from-sleep states, and allocates the framebuffer (1872×1404÷2 bytes = ~1.3 MB) in PSRAM.
- On `update()`: if the IT8951 is in inter-refresh sleep (flag `it8951_sleeping_`), raises GPIO11 (EPD_Drive_EN), waits 10 ms for the TPS22916 switch, then issues SYS_RUN (0x0001) to wake the IT8951. Then runs the ESPHome draw callback into the framebuffer, detects whether the image is pure binary (all pixels 0x00 or 0x0F) to pick the faster 1bpp upload path vs 4bpp grayscale, sets the ambient temperature on the controller for waveform selection, issues an INIT clear pass (mode 0) to eliminate ghosting, then a GC16 refresh (mode 2). After the refresh completes (LUTAFSR=0), issues SLEEP (0x0003) and drives GPIO11 LOW to cut EPD_Drive power — saving the TPS651851RSLR quiescent current for the remainder of the wake window.
- **Power note**: the IT8951 auto-manages POWER_SEQ (EPD panel power on/off) — it powers on when DISPLAY_AREA is called and powers off when the display finishes (LUTAFSR=0). No explicit POWER_SEQ command is needed. SLEEP/SYS_RUN are for the IT8951 clock domain only; GPIO11 (EPD_Drive_EN) is cut separately after SLEEP to eliminate the TPS651851RSLR input quiescent draw. GPIO21 (ITE_VCC_EN / 1.8 V core) remains HIGH across refreshes so the probe sequence is not needed between images in interactive mode.

**`reference/`** — gitignored PDFs and example C code from ITE/Seeed; context only, never committed.

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
- **0x44** — SHT40 temperature & humidity sensor
- **0x51** — PCF8563 RTC
- SY6974B PMIC (address per datasheet; INT line connected but GPIO not labeled in schematic)
- Touch panel (address per touch IC datasheet)
- TPS651851RSLR EPD PMIC is controlled by IT8951 over a separate internal I2C path (not directly from ESP32)

### SPI bus sharing
GPIO7/8/9 (SCK/MISO/MOSI) are shared between the IT8951 (CS=GPIO10) and the SD card (CS=GPIO14). Both must never be chip-selected simultaneously. The IT8951 component and any SD card driver must coordinate CS lines.

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
- Green user LED: GPIO16
- Buzzer: MLT-8530 on GPIO45
- Dual PDM stereo microphone: GPIO38 (enable), GPIO41 (data), GPIO42 (clock)
- MicroSD slot: SPI on GPIO7/8/9/14, detect GPIO15, power GPIO39
- Touch panel FPC (J3, 8-pin): I2C0, INT=GPIO2, RST=GPIO48 (not used in art-frame config)
- 6-pin 2.54mm header (J2): includes HEADER_3V3 (GPIO46), GPIO47, GPIO6/ADC, I2C0

## Key constraints

- `vcom` in `config.yaml` must match the panel's physical VCOM voltage (default 1400 = 1.400 V). Wrong VCOM causes poor contrast or display damage over time.
- Images must be PNG, 16-level grayscale, and sized exactly to the rotation: 1404×1872 (portrait/270°) or 1872×1404 (landscape/0°). Files must be sequentially numbered `1.png`, `2.png`, … with no gaps.
- ESPHome `min_version: 2025.11.0` is required (uses `online_image` component features added around that version).
- The component uses `MALLOC_CAP_SPIRAM` — PSRAM must be present and enabled (`psram: mode: octal`).

## IT8951 driver protocol reference

Source: ITE Tech IT8951 Programming Guide v2.7 (2017-09-04), applicable to IT8951 CX/DX variant used on this board.

### SPI protocol (CX/DX variant)

The IT8951E/DX translates SPI frames into internal I80 bus cycles. All commands and parameters are 16-bit words.

**SPI electrical spec:** Mode 0 only (CPOL=0, CPHA=0). Suggested clock 12 MHz, max 24 MHz. Big-endian byte order per word (send byte[15:8] first). ESP32 is little-endian, so each 16-bit word must be byte-swapped before transmission.

**Preamble values** (first word of every SPI transaction sets the cycle type):
| Preamble | I80 cycle type |
|----------|---------------|
| 0x6000 | Write command code |
| 0x0000 | Write data |
| 0x1000 | Read data |

**Read protocol quirk:** Every read transaction returns one dummy word first that must be discarded. If reading ≥2 words, keep CS low for the entire burst — multiple single-word reads are not valid for data ≥2 words.

**HRDY / busy signal:** Poll HRDY (GPIO13, active-low in hardware but wired as BUSY) before every SPI command or data word. The IT8951 FIFO is 2 KB (1024 words); burst transfers must be chunked to ≤1024 words at a time.

### Host commands (all 16-bit codes)

| Code | Name | Description |
|------|------|-------------|
| 0x0001 | SYS_RUN | Enable all clocks, go to idle |
| 0x0003 | SLEEP | Disable all clocks |
| 0x0010 | REG_RD | Read register; send address, then read 16-bit value |
| 0x0011 | REG_WR | Write register; send address then value |
| 0x0012 | MEM_BST_RD_T | Set burst-read address+length (4 params: addr[15:0], addr[25:16], cnt[15:0], cnt[25:16]) |
| 0x0013 | MEM_BST_RD_S | Start burst-read data fetch (no params; follow MEM_BST_RD_T) |
| 0x0014 | MEM_BST_WR | Set burst-write address+length (same 4 params); **do not use during LD_IMG** |
| 0x0015 | MEM_BST_END | End burst read or write cycle — mandatory after every burst |
| 0x0020 | LD_IMG | Load full image; 1 param = ARG (endian/bpp/rotate packed word) |
| 0x0021 | LD_IMG_AREA | Load partial image; 5 params = ARG, x, y, w, h |
| 0x0022 | LD_IMG_END | End load image cycle — **mandatory after every LD_IMG/LD_IMG_AREA** |
| 0x0302 | GET_DEV_INFO | Returns 20 words: panel W, panel H, imgbuf addr L, imgbuf addr H, 8×FW version, 8×LUT version |
| 0x0034 | DISPLAY_AREA | Trigger display refresh; 5 params = x, y, w, h, mode |
| 0x0038 | POWER_SEQ | EPD power on/off: par[0]=0 off, 1 on — **auto-managed**: IT8951 issues this automatically on DISPLAY_AREA and again when refresh finishes; do not call manually unless overriding sequencing |
| 0x0039 | SET_VCOM | Get/set VCOM: par[0]=0 get, par[0]=1 set then par[1]=value (e.g. 1400 = −1.400 V) |
| 0x0040 | SET_TEMP | Force temperature: par[0]=0 get, par[0]=1 set then par[1]=°C; disables IT8951 thermal sensor |

**LD_IMG ARG word bit layout:**
- Bit 8: endian (0=little, 1=big)
- Bits 5:4: bpp (00=2bpp, 01=3bpp, 10=4bpp, 11=8bpp)
- Bits 1:0: rotate (00=0°, 01=90°, 10=180°, 11=270°)

### Key register addresses

| Address | Name | Purpose |
|---------|------|---------|
| 0x0004 | I80CPCR | Set to 1 at init to enable I80 parameter-packed mode (strongly recommended) |
| 0x0208 | LISAR | Load Image Start Address Register (26-bit, split: write low 16 bits, then high bits to LISAR+2=0x020A) |
| 0x1134 | UP0SR | Update Parameter 0 |
| 0x1138 | UP1SR | Update Parameter 1; bit 18 (= bit 2 of UP1SR+2) enables 1bpp display mode |
| 0x1224 | LUTAFSR | LUT engine status; 0 = all free, non-zero = busy; poll before issuing DISPLAY_AREA |
| 0x1250 | BGVR | 1bpp color table; bits[7:0]=foreground gray level, bits[15:8]=background gray level |

### Initialization sequence

1. Issue SYS_RUN (0x0001).
2. Send GET_DEV_INFO (0x0302); receive 20 words. Extract panel dimensions and image buffer address. Recommended: drop SPI to 2 MHz for this call only, then restore to 12 MHz.
3. Write I80CPCR (0x0004) = 1 to enable parameter-packed mode.
4. Store the image buffer base address for all subsequent LD_IMG calls.
5. Optionally call SET_VCOM (0x0039) to verify or correct VCOM against the panel label.

### Load image sequence

1. Write LISAR (0x0208) = imgBufAddr[15:0], then LISAR+2 (0x020A) = imgBufAddr[25:16]. Repeat before every load.
2. Send LD_IMG (0x0020) + ARG, or LD_IMG_AREA (0x0021) + ARG + x + y + w + h.
3. Transfer pixel data words.
4. Send LD_IMG_END (0x0022) — **always required**, even if transfer was interrupted.

**Alignment constraint for LD_IMG_AREA:** X start position must be a multiple of 8 (2bpp), 4 (4bpp), or 2 (8bpp). If X is not aligned, pad dummy pixels before the first real pixel and after the last real pixel so both X-start and X-end land on a boundary. The IT8951 ignores the dummy pixels but the transfer length must include them.

**2048-pixel limit:** Maximum image dimension per LD_IMG command is 2048 pixels in either axis. For the 1872-wide panel this is fine; if ever loading strips taller than 2048 px, split into two calls.

### Display mode numbers

| Mode | Name | Use |
|------|------|-----|
| 0 | INIT | Full clear to white; eliminates ghosting; slow |
| 1 | DU | Direct update; fast binary black/white |
| 2 | GC16 | 16-level grayscale; high quality; slow |
| 3 | GL16 | 16-level grayscale, optimized for text |
| 4 | GLR16 | GL16 with ghost removal |

The driver uses mode 0 (INIT clear) followed by mode 2 (GC16) for each frame, which eliminates ghosting at the cost of two full refresh passes.

**DISPLAY_AREA (0x0034) does not wait internally for the LUT engine.** The host must poll LUTAFSR (0x1224) == 0 before issuing this command.

### 1bpp mode

Used for binary (pure black/white) images — faster to load and refresh.

1. Enable: `WriteReg(UP1SR+2, ReadReg(UP1SR+2) | (1<<2))`
2. Set colors: `WriteReg(BGVR, (background_gray << 8) | foreground_gray)` — gray levels 0x00–0xFF map to G0–G15.
3. Load image using LD_IMG_AREA but divide both X and Width by 8 before passing to the command.
4. Issue DISPLAY_AREA, wait for LUTAFSR == 0.
5. Restore: `WriteReg(UP1SR+2, ReadReg(UP1SR+2) & ~(1<<2))`

### Power state management

**POWER_SEQ auto-management (source: Programming Guide v2.7, §1.2):** IT8951 automatically issues POWER_SEQ on when DISPLAY_AREA is called and POWER_SEQ off when the refresh finishes (LUTAFSR returns to 0). The host does not need to call 0x0038 explicitly. The CLAUDE.md warning about cutting GPIO11 without POWER_SEQ applies to cutting the external power switch *without* IT8951's involvement — but IT8951 already handles the sequenced shutdown before LUTAFSR clears.

**SLEEP (0x0003) / SYS_RUN (0x0001):** These operate on the IT8951 clock domain only and do not affect the EPD panel power rails (those are controlled by auto POWER_SEQ via IT8951→TPS651851RSLR I2C). Issue SLEEP after a refresh completes to stop IT8951 internal clocks; issue SYS_RUN to wake before the next refresh. No parameters for either command.

**HRDY during SLEEP:** HRDY remains HIGH when IT8951 is in SLEEP state. The ITE sample code for waking from SLEEP calls `LCDWaitForReady()` then `LCDWriteCmdCode(SYS_RUN)` — this would hang if HRDY went LOW during sleep. Reliable SYS_RUN wake therefore does not require a hardware RST toggle, enabling fast inter-refresh wakeup without the full `power_cycle_()` probe sequence.

**GPIO11 (EPD_Drive_EN) vs SLEEP:** SLEEP stops IT8951 clocks but the IT8951 1.8 V core (GPIO21/ITE_VCC_EN) stays on. GPIO11 can be driven LOW after SLEEP to cut TPS651851RSLR input quiescent current; raise it again (with a 10 ms stabilisation delay for the TPS22916 switch) before sending SYS_RUN on the next refresh.

### Temperature override

Command 0x0040 with par[0]=1 and par[1]=temperature in °C overrides IT8951's internal thermal sensor. Once set, IT8951 stops reading its own sensor. The E1003 board has a SHT40 (GPIO19/20, I2C 0x44) that is more accurate; the driver reads it and injects the value before each refresh to ensure correct waveform selection.

## PCF8563 RTC protocol reference

Source: NXP PCF8563 product data sheet Rev. 11.1 (19 January 2026), `reference/PCF8563.pdf`.

### Overview

I2C target address: write 0xA2 (7-bit: 0x51), read 0xA3. Max I2C speed 400 kHz. Register address auto-increments after each byte, so a full time read/write is one burst. All time fields are **BCD-encoded**.

### Register map (relevant subset)

| Address | Name | Key bits |
|---------|------|----------|
| 0x00 | Control_status_1 | Bit 5: STOP — set 1 to halt clock (for time-setting), clear to run |
| 0x02 | VL_seconds | Bit 7: VL flag; bits 6:0: seconds (BCD 0–59) |
| 0x03 | Minutes | Bits 6:0: minutes (BCD 0–59) |
| 0x04 | Hours | Bits 5:0: hours (BCD 0–23) |
| 0x05 | Days | Bits 5:0: day of month (BCD 1–31) |
| 0x06 | Weekdays | Bits 2:0: weekday (0=Sun … 6=Sat) |
| 0x07 | Century_months | Bit 7: century flag C; bits 4:0: month (BCD 1–12) |
| 0x08 | Years | Bits 7:0: year within century (BCD 00–99; e.g. 2026 → 0x26) |

### VL flag (Voltage-Low) — critical for sleep scheduling

Bit 7 of register 0x02 (VL_seconds). **Default/startup value is 1** — set whenever VDD drops below ~0.9–1.0 V (e.g., coin cell dead, first boot after assembly). While VL=1 the clock has been running but integrity of the time value is not guaranteed.

The ESPHome `pcf8563` component's `now().is_valid()` returns **false** when VL is set. This is why the `on_shutdown` lambda falls back to sleeping one full interval when `!t.is_valid()`:
- First boot with no HA time sync yet → VL=1 → fallback sleep (correct)
- After `pcf8563.write_time` is called (triggered by `on_time_sync`) → ESPHome driver clears VL → subsequent wakes use real RTC time

VL can only be cleared by writing 0 to bit 7 via I2C; the ESPHome driver does this automatically during `write_time`.

### Reading time atomically

Seconds through years (0x02–0x08) must be read in a **single I2C burst** within 1 second. Reading in separate transactions risks a rollover between reads, giving minutes from one moment and hours from the next. The watchdog timer releases the interface (losing 1 s) if an I2C transaction exceeds 1 s.

To read: write start address 0x02, then read 7 bytes (VL_seconds, Minutes, Hours, Days, Weekdays, Century_months, Years) without releasing the bus.

### Setting time

1. Write STOP=1 (Control_status_1 bit 5) to halt the prescaler.
2. Write registers 0x02–0x08 with BCD-encoded values; also clear VL (bit 7 of 0x02) to mark time as valid.
3. Write STOP=0 to restart the clock. First tick occurs 0.5–1.0 s after STOP release (prescaler uncertainty).

The ESPHome `pcf8563.write_time` action handles all of this.

### Century flag

Bit 7 of register 0x07 (Century_months) is toggled automatically when the Years register overflows from 99→00. The initial century assignment is user-defined; ESPHome treats the year as 2000+years when C=0 and 2100+years when C=1 (implementation detail — check the ESPHome pcf8563 component source if century tracking matters).

### Backup power

Backup current from CR1220 coin cell: ~0.25 µA typical at 3.0 V (CLKOUT disabled). A CR1220 (~36 mAh) gives theoretical backup of several years; practical lifetime is limited more by self-discharge (~5–10 years) than by RTC current draw. The RTC continues running during ESP32 deep sleep as long as VSYS is present or the coin cell is live.

## ED103TC2 panel reference

Source: E Ink Holdings P-511-828(V:1), Formal Spec V1.0 (2019-05-14), `reference/Eink P-511-828-V1_ED103TC2 Formal Spec V1.0_20190514.pdf`.

### Physical specifications

- **Active area**: 157.25 mm (V) × 209.66 mm (H)
- **Outline**: 174.4 × 216.7 × 0.78 mm, weight 60 ±7 g
- **Pixel pitch**: 112 µm × 112 µm (square pixels)
- **Resolution**: 1404 (V) × 1872 (H) — panel is landscape-oriented natively
- **Grayscale**: 2–16 levels depending on controller waveform file

### Native scan direction

The panel's internal scan origin is at the **top-right corner** of the landscape panel: (0,0) = top-right, (1872,0) = top-left, (0,1404) = bottom-right. The FPC connector exits from the bottom edge. This is why the IT8951 is configured with rotation=270° (bits 1:0 = `11` in the LD_IMG ARG word) — it rotates the image so portrait-oriented artwork fills the screen correctly. When the IT8951 reports panel dimensions via GET_DEV_INFO, it returns 1872×1404 (landscape native); the driver maps this to portrait by rotating in firmware.

### Temperature limits

- **Operating**: 0°C to +50°C — must not refresh outside this range; the SHT40 temperature injection exists partly to allow the IT8951 to select correct waveforms within this window
- **Storage**: −25°C to +70°C

### Power supply rails

All generated by the TPS651851RSLR PMIC, controlled by IT8951 over I2C:

| Rail | Typ | Tolerance | Function |
|------|-----|-----------|----------|
| VDD | 3.3 V | 3.0–3.6 V | Digital logic |
| VPOS | +15 V | ±0.4 V | Source driver positive |
| VNEG | −15 V | ±0.4 V | Source driver negative |
| VGH | +28 V | ±1 V | Gate driver positive |
| VGL | −20 V | ±1 V | Gate driver negative |
| VCOM | adjusted | ±0.1 V from label | Common electrode |

**VCOM tolerance is ±0.1 V from the value printed on the panel label.** The default `vcom: 1400` in `config.yaml` (= −1.400 V) must be changed to match the actual panel if a replacement is fitted.

**VCOM inrush current**: up to 0.8 A at turn-on — this is why VCOM is applied last in the power-on sequence.

### Power-on sequence

Managed by IT8951 via TPS651851RSLR. Must follow this order to avoid panel damage:
1. Source chain: VSS → VDD → VNEG → VPOS → VCOM
2. Gate chain: VSS → VDD → VGL → VGH (can parallel with source chain after VDD is stable)

Key minimum delays: Tep ≥ 1 ms (VNEG stable before VPOS rises), Tng ≥ 1 ms (VGL stable before VGH rises).

### Power-off sequence — critical constraints

Violating these can permanently damage the panel:
1. Stop data signals, then wait ≥100 µs before dropping VCOM.
2. Drop VPOS and VNEG simultaneously (or VPOS first). They discharge through internal pull-down resistors.
3. **VGL must stay negative of VCOM throughout the entire discharge period** — do not cut VGL early.
4. Turn off VGL only after VNEG and VPOS have fully discharged to GND.
5. Allow ≥500 ms total discharge time (Ted ≥ 0.5 s, discharged to −7.4 V reference point).

The IT8951 POWER_SEQ command (0x0038 with par[0]=0) handles this sequence through the TPS651851RSLR. Cutting GPIO11 (EPD_Drive_EN) without issuing POWER_SEQ first bypasses the sequencing and risks panel damage.

### Power consumption

- **Active refresh** (typical → max): 380 mW → 5.5 W — drives the decision to gate EPD_Drive_EN between refreshes
- **Standby** (image held, controller idle): ≤1.22 mW — E-ink is bi-stable; no power is needed to hold an image

### Optical characteristics (at 25°C)

- **White reflectance**: 35% min, 43% typical
- **Contrast ratio**: 10 min, 14 typical
- **Surface**: anti-glare treatment on protective sheet

### FPC connector pinout (selected pins)

Connector type 196033-40041, 40 pins. The schematic routes board connector J8 pin 1 to panel pin 6 (an offset of 5), so board J8 pin N = panel pin N+5.

| Panel pin | Signal | Description |
|-----------|--------|-------------|
| 5, 11 | VDD | Digital supply (3.3 V) |
| 9, 12, 22 | VSS | Ground |
| 10 | VCOM | Common electrode voltage |
| 36 | VPOS | Source driver positive (+15 V) |
| 38 | VNEG | Source driver negative (−15 V) |
| 40 | Border | Border electrode (tied to VCOM level for uniform border) |
| 34 | TEST | E Ink internal test pin — must be connected to VDD |

### Handling notes

- Do not clean the protective sheet (PS) with acetone, toluene, or alcohol — use petroleum benzene or normal-hexane
- Do not touch the PS surface with bare hands or greasy cloth; wipe water or saliva off immediately
- Operating humidity limit for reliability testing: 90% RH at 40°C for 168 h
