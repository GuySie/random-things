"""Shared constants and validators for the ``epaper_spectra6_133`` component."""

import esphome.codegen as cg
from esphome.components import display as display_core
import esphome.config_validation as cv

CONF_SPI_HOST = "spi_host"
CONF_CS0_PIN = "cs0_pin"
CONF_CS1_PIN = "cs1_pin"
CONF_CLK_PIN = "clk_pin"
CONF_DATA0_PIN = "data0_pin"
CONF_DATA1_PIN = "data1_pin"
CONF_BUSY_PIN = "busy_pin"
CONF_RESET_PIN = "reset_pin"
CONF_POWER_PIN = "power_pin"
CONF_CHANGE_DETECTION_MODE = "change_detection_mode"
CONF_REFRESH_MODE = "refresh_mode"
# Deprecated YAML option kept for pre-1.0 compatibility. Remove in version 1.0.
CONF_UPDATE_MODE = "update_mode"
CONF_CLEAR_COLOR = "clear_color"
CONF_AUTO_SLEEP = "auto_sleep"
CONF_POWER_OFF_AFTER_SLEEP = "power_off_after_sleep"

# ESP-IDF SPI host enum aliases. Raw integer values are also accepted.
_SPI_HOST_NAMES: dict[str, int] = {
    "SPI1_HOST": 0,
    "SPI2_HOST": 1,
    "SPI3_HOST": 2,
}


def _validate_spi_host(value) -> int:
    """Validate ``spi_host`` and return the corresponding ESP-IDF host number."""
    if isinstance(value, int):
        if value < 0:
            raise cv.Invalid("SPI host number must be non-negative")
        return value
    upper = cv.string_strict(value).upper()
    if upper in _SPI_HOST_NAMES:
        return _SPI_HOST_NAMES[upper]
    raise cv.Invalid(
        f"Unknown SPI host '{value}'. "
        f"Valid names: {', '.join(_SPI_HOST_NAMES.keys())} — or pass a raw integer."
    )


epaper_spectra6_133_ns = cg.esphome_ns.namespace("epaper_spectra6_133")
EpaperSpectra6133 = epaper_spectra6_133_ns.class_(
    "EpaperSpectra6133", display_core.DisplayBuffer
)
ChangeDetectionMode = epaper_spectra6_133_ns.enum("ChangeDetectionMode")
RefreshMode = epaper_spectra6_133_ns.enum("RefreshMode")
