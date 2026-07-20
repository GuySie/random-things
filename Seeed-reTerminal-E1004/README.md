# Seeed reTerminal E1004 Art Display

ESPHome configuration for using a [Seeed reTerminal E1004](https://www.seeedstudio.com/reTerminal-E1004-p-6692.html) epaper display as a digital art frame with Home Assistant. The 13.3-inch E Ink Spectra 6 panel (1200×1600px) renders full-color images, making this display well suited to emulate the look of a color print or poster.

The display wakes from deep sleep on a configurable schedule, loads a random image from a configurable folder in your Home Assistant local www folder, displays it, and returns to deep sleep. By default, recent images are tracked across sleep cycles so the same image is not shown again until roughly half the collection has cycled through — this can be toggled off from Home Assistant to pick images with no restriction. The left/right buttons on the device (front touch or back physical) step through images sequentially while awake; the refresh button wakes the display early and loads a random image once wifi connects.

The refresh schedule — how often to wake and during which hours — and the image folder are all configured directly from Home Assistant without reflashing.

### Installation

Requires **ESPHome 2026.7.0 or newer** — the first release with built-in support for the E1004's T133A01 display controller. Unlike the E1003, no custom display component is needed.

The `components` folder and `image_history.h` in this repo must be placed in the **same directory** as the device's YAML file (`config.yaml` here) before you compile or flash. ESPHome resolves the `local` path relative to the config file. If you are using the **ESPHome dashboard** (Home Assistant app or standalone), copy them into your ESPHome config directory. Full step-by-step instructions are in the comments at the top of [config.yaml](config.yaml).

The `components` folder contains a copy of ESPHome's built-in `epaper_spi` component with a bug fix for an intermittent deadlock at the end of the display data transfer that reboots the device mid-update. It can be removed once the fix lands in an ESPHome release.

## Image preparation

Images must be:
- PNG format
- **Portrait** (rotation: 0): 1200×1600 px
- **Landscape** (rotation: 90): 1600×1200 px
- Dithered to the 6-color E Ink Spectra 6 palette: black `#000000`, white `#FFFFFF`, red `#FF0000`, green `#00FF00`, blue `#0000FF`, yellow `#FFFF00`

Number images sequentially with no gaps: `1.png`, `2.png`, `3.png`, etc.

The display hard-snaps any other color to the nearest of these six, which looks poor — dither on your computer first, for example with [Ditherit](https://ditherit.com/) using the palette above.

## Differences from the E1003 configuration

- Uses ESPHome's `epaper_spi` display platform instead of a local IT8951 component; the local `components` folder is only a temporarily patched copy of that upstream platform.
- The E1003's deep sleep GPIO hold workarounds are not needed: on the E1004 every peripheral power switch has a hardware pulldown, so everything turns off by design when the ESP32 sleeps.
- A Spectra 6 color refresh takes about 30 seconds and runs in the background, so the device waits a configurable delay (default 60s) after starting the refresh before entering deep sleep instead of sleeping immediately.

## AI warning

While this is based on an original configuration for the Seeed reTerminal E1002 that I wrote manually, this version for the E1004 was made with heavy assistance from Claude Code. If you are against using AI-generated code please do not install this.
