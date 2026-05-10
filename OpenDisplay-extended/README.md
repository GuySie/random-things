# opendisplay-extended

A Home Assistant custom component that adds extra actions on top of the upstream [OpenDisplay](https://www.home-assistant.io/integrations/opendisplay/) integration.

This component is fully made by Claude Code, not intended for production use, and not intended to replace future OpenDisplay functionality. It is a stopgap measure to add two actions for ease of use of OpenDisplay devices. 

## What it does

- `opendisplay_extended.upload_random_image` — pick a media folder, get a random image, delegate to `opendisplay.upload_image`.
- `opendisplay_extended.upload_image_from_url` — provide an HTTP/HTTPS URL, download the image, delegate to `opendisplay.upload_image`.

## Requirements

- Home Assistant with the [OpenDisplay integration](https://www.home-assistant.io/integrations/opendisplay/) installed and configured.
- No additional Python packages required — this component has no dependencies beyond what the upstream integration already provides.

## Installation

1. Copy `custom_components/opendisplay_extended/` into your Home Assistant config `custom_components/` directory.
2. Add the following to your `configuration.yaml`:
   ```yaml
   opendisplay_extended:
   ```
3. Restart Home Assistant.

## Usage

Developer Tools → Actions

### `upload_random_image`

| Field | Description |
|---|---|
| `device_id` | HA device registry ID |
| `folder` | Media folder to pick from (select any image inside it) |
| `rotation` / `dither_mode` / `refresh_mode` / `fit_mode` / `tone_compression` | Passed through to `opendisplay.upload_image` |

### `upload_image_from_url`

| Field | Description |
|---|---|
| `device_id` | HA device registry ID |
| `image_url` | HTTP or HTTPS URL of the image to upload |
| `rotation` / `dither_mode` / `refresh_mode` / `fit_mode` / `tone_compression` | Passed through to `opendisplay.upload_image` |
