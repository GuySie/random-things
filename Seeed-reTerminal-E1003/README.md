# Seeed reTerminal E1003 Art Display

ESPHome configuration for using a [Seeed reTerminal E1003](https://www.seeedstudio.com/reTerminal-E1003-frame-bundle.html) epaper display as a digital art frame with Home Assistant. Combining the high DPI (1872x1404px resolution) with 16 tints grayscale, this display can convincingly emulate the look of a black and white photography print.

The display wakes from deep sleep on a configurable schedule, loads a random image from a configurable folder in your Home Assistant local www folder, displays it, and immediately returns to deep sleep. By default, recent images are tracked across sleep cycles so the same image is not shown again until roughly half the collection has cycled through — this can be toggled off from Home Assistant to pick images with no restriction. The white buttons on the device step through images sequentially while awake; the green button wakes the display early and loads a random image once wifi connects.

The refresh schedule — how often to wake and during which hours — and the image folder are all configured directly from Home Assistant without reflashing.

### Installation

The `components` folder and `image_history.h` in this repo must be placed in the **same directory** as the device's YAML file (`config.yaml` here) before you compile or flash. ESPHome resolves the `local` path relative to the config file. If you are using the **ESPHome dashboard** (Home Assistant app or standalone), copy them into your ESPHome config directory. Full step-by-step instructions are in the comments at the top of [config.yaml](config.yaml). 

## Image preparation

Images must be:
- PNG format
- **Portrait** (rotation: 270): 1404×1872 px
- **Landscape** (rotation: 0): 1872×1404 px
- 16-level grayscale (the IT8951 controller renders 4bpp grayscale)

Number images sequentially with no gaps: `1.png`, `2.png`, `3.png`, etc.

## IT8951 component

ESPHome currently does not provide support for the IT8951 controller used in the E1003, so we provide a local component. This component is a modification of the [code by koosoli](https://github.com/esphome/esphome/pull/15415), modified using Claude Code to work specifically for this art frame purpose. No guarantees it will work for anything else.

## Deep sleep optimization

Some great investigative work by [u/ar0v3r](https://www.reddit.com/r/homeassistant/comments/1tqy14f/comment/op6c8i9/) revealed that the SD-card and touch controllers were still drawing power during deep sleep. I've implemented their fixes in v0.8.0 of this configuration and have cut power consumption during deep sleep by half! Many thanks to u/ar0v3r.

## AI warning

While this is based on an original configuration for the Seeed reTerminal E1002 that I wrote manually, this version for the E1003 was made with heavy assistance from Claude Code. If you are against using AI-generated code please do not install this.
