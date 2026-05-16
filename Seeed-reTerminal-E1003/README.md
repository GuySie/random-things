# Seeed reTerminal E1003 Art Display

ESPHome configuration for using a [Seeed reTerminal E1003](https://www.seeedstudio.com/reTerminal-E1003-frame-bundle.html) epaper display as a digital art frame with Home Assistant. Combining the high DPI (1872x1404px resolution) with 16 tints grayscale, this display can convincingly emulate the look of a black and white photography print.

The display wakes from deep sleep on a configurable schedule, loads a random image from your Home Assistant media folder, and goes back to sleep. By default, recent images are tracked across sleep cycles so the same image is not shown again until roughly half the collection has cycled through — this can be toggled off from Home Assistant to pick images with no restriction. The white buttons on the device step through images sequentially while awake; the green button wakes the display early and loads a random image once wifi connects.

The refresh schedule — how often to wake and during which hours — is configured directly from Home Assistant without reflashing.

### Installing the component

The `components` folder and `image_history.h` in this repo must be placed in the **same directory** as `config.yaml` before you compile or flash. ESPHome resolves the `local` path relative to the config file. If you are using the **ESPHome dashboard** (Home Assistant add-on or standalone), copy them into your ESPHome config directory.

## Setup

Full step-by-step instructions are in the comments at the top of [config.yaml](config.yaml). The short version:

1. Set the substitutions at the top of `config.yaml` (name, API key, OTA password, etc.)
2. Create a Home Assistant helper boolean to control deep sleep
3. Flash the configuration and add the device to Home Assistant
4. Upload your images and set the **Total Images** number in the device settings
5. Set **Refresh Interval**, **Active Hours Start**, and **Active Hours End** in the device settings to configure the wakeup schedule (no reflash needed)

## Image preparation

Images must be:
- PNG format
- **Portrait** (rotation: 270): 1404×1872 px
- **Landscape** (rotation: 0): 1872×1404 px
- 16-level grayscale (the IT8951 controller renders 4bpp grayscale)

Number images sequentially with no gaps: `1.png`, `2.png`, `3.png`, etc.

## IT8951 component

ESPHome currently does not provide support for the IT8951 controller used in the E1003, so we provide a local component. This component is a modification of the [code by koosoli](https://github.com/esphome/esphome/pull/15415), modified using Claude Code to work specifically for this art frame purpose. No guarantees it will work for anything else.
