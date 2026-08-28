"""ESPHome display platform schema and code generation for this component."""

import logging

import esphome.codegen as cg
from esphome import pins
from esphome.components import display as display_core
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_LAMBDA

from . import (
    CONF_AUTO_SLEEP,
    CONF_BUSY_PIN,
    CONF_CHANGE_DETECTION_MODE,
    CONF_CLEAR_COLOR,
    CONF_CLK_PIN,
    CONF_CS0_PIN,
    CONF_CS1_PIN,
    CONF_DATA0_PIN,
    CONF_DATA1_PIN,
    CONF_POWER_PIN,
    CONF_POWER_OFF_AFTER_SLEEP,
    CONF_RESET_PIN,
    CONF_SPI_HOST,
    CONF_REFRESH_MODE,
    CONF_UPDATE_MODE,
    EpaperSpectra6133,
    _validate_spi_host,
)

AUTO_LOAD = ["display"]
DEPENDENCIES = ["esp32"]

_LOGGER = logging.getLogger(__name__)

_CHANGE_DETECTION_MODES = {
    "track": cg.RawExpression(
        "esphome::epaper_spectra6_133::ChangeDetectionMode::TRACK"
    ),
    "compare": cg.RawExpression(
        "esphome::epaper_spectra6_133::ChangeDetectionMode::COMPARE"
    ),
}

_REFRESH_MODES = {
    "full": cg.RawExpression("esphome::epaper_spectra6_133::RefreshMode::FULL"),
    "partial": cg.RawExpression("esphome::epaper_spectra6_133::RefreshMode::PARTIAL"),
}

_CLEAR_COLORS = {
    "black": cg.RawExpression("esphome::epaper_spectra6_133::EpaperSpectra6133::BLACK"),
    "white": cg.RawExpression("esphome::epaper_spectra6_133::EpaperSpectra6133::WHITE"),
    "yellow": cg.RawExpression(
        "esphome::epaper_spectra6_133::EpaperSpectra6133::YELLOW"
    ),
    "red": cg.RawExpression("esphome::epaper_spectra6_133::EpaperSpectra6133::RED"),
    "blue": cg.RawExpression("esphome::epaper_spectra6_133::EpaperSpectra6133::BLUE"),
    "green": cg.RawExpression("esphome::epaper_spectra6_133::EpaperSpectra6133::GREEN"),
}


def _migrate_update_mode(config):
    """Accept deprecated update_mode and map it to refresh_mode."""
    # Deprecated YAML compatibility path. Remove update_mode support in version 1.0.
    if CONF_UPDATE_MODE not in config:
        return config
    if CONF_REFRESH_MODE in config:
        raise cv.Invalid(
            "'update_mode' is deprecated and cannot be used together with "
            "'refresh_mode'. Use 'refresh_mode' instead."
        )
    config = config.copy()
    config[CONF_REFRESH_MODE] = config.pop(CONF_UPDATE_MODE)
    _LOGGER.warning(
        "'update_mode' is deprecated and will be removed in version 1.0; "
        "use 'refresh_mode' instead"
    )
    return config


CONFIG_SCHEMA = cv.All(
    _migrate_update_mode,
    display_core.FULL_DISPLAY_SCHEMA.extend(
        {
            # Component ID used by lambdas and automations to call the public display API.
            cv.GenerateID(): cv.declare_id(EpaperSpectra6133),
            # ESP-IDF SPI host. Accepts SPI2_HOST, SPI3_HOST, or a raw non-negative
            # host number; defaults to SPI3_HOST to match the board examples.
            cv.Optional(CONF_SPI_HOST, default="SPI3_HOST"): _validate_spi_host,
            # Left controller chip-select GPIO. Must be an internal output-capable pin.
            cv.Required(CONF_CS0_PIN): pins.internal_gpio_output_pin_number,
            # Right controller chip-select GPIO. Must be an internal output-capable pin.
            cv.Required(CONF_CS1_PIN): pins.internal_gpio_output_pin_number,
            # SPI clock GPIO. Must be an internal output-capable pin.
            cv.Required(CONF_CLK_PIN): pins.internal_gpio_output_pin_number,
            # SPI MOSI GPIO for the controller data path. Must be output-capable.
            cv.Required(CONF_DATA0_PIN): pins.internal_gpio_output_pin_number,
            # Secondary SPI data GPIO used by the panel transport. Must be output-capable.
            cv.Required(CONF_DATA1_PIN): pins.internal_gpio_output_pin_number,
            # Shared panel BUSY GPIO. Must be input-capable; refresh and sleep flows wait
            # for this signal before issuing timing-sensitive controller commands.
            cv.Required(CONF_BUSY_PIN): pins.internal_gpio_input_pin_number,
            # Hardware reset GPIO. Must be output-capable and is used during setup and
            # cold wake after deep sleep.
            cv.Required(CONF_RESET_PIN): pins.internal_gpio_output_pin_number,
            # Panel load-switch GPIO. Must be output-capable; normally held on and driven
            # low only after sleep when power_off_after_sleep is enabled.
            cv.Required(CONF_POWER_PIN): pins.internal_gpio_output_pin_number,
            # Changed-pixel detection for partial update(). Valid values are track
            # (default, records pixel writes) and compare (frame comparison, extra PSRAM).
            cv.Optional(CONF_CHANGE_DETECTION_MODE, default="track"): cv.one_of(
                *_CHANGE_DETECTION_MODES, lower=True
            ),
            # Refresh mode used by update(). Valid values are full (default, full frame)
            # and partial (refresh only the detected changed region).
            cv.Optional(CONF_REFRESH_MODE, default="full"): cv.one_of(
                *_REFRESH_MODES, lower=True
            ),
            # Colour used by clear() and ESPHome's auto_clear_enabled pre-render
            # clear. Limited to the panel palette; defaults to white for e-paper.
            cv.Optional(CONF_CLEAR_COLOR, default="white"): cv.one_of(
                *_CLEAR_COLORS, lower=True
            ),
            # Boolean. When true (default), successful refresh operations automatically send
            # the panel deep-sleep command; the next display operation wakes it first.
            cv.Optional(CONF_AUTO_SLEEP, default=True): cv.boolean,
            # Boolean. When true, deep sleep also switches power_pin low after the panel
            # enters sleep; defaults false because the next operation performs a cold wake.
            cv.Optional(CONF_POWER_OFF_AFTER_SLEEP, default=False): cv.boolean,
        }
    ).extend(cv.polling_component_schema("never")),
)


async def to_code(config):
    """Generate C++ setup code for one ``epaper_spectra6_133`` display."""
    var = cg.new_Pvariable(config[CONF_ID])
    await display_core.register_display(var, config)

    cg.add(var.set_spi_host(config[CONF_SPI_HOST]))
    cg.add(var.set_cs0_pin(config[CONF_CS0_PIN]))
    cg.add(var.set_cs1_pin(config[CONF_CS1_PIN]))
    cg.add(var.set_clk_pin(config[CONF_CLK_PIN]))
    cg.add(var.set_data0_pin(config[CONF_DATA0_PIN]))
    cg.add(var.set_data1_pin(config[CONF_DATA1_PIN]))
    cg.add(var.set_busy_pin(config[CONF_BUSY_PIN]))
    cg.add(var.set_reset_pin(config[CONF_RESET_PIN]))
    cg.add(var.set_power_pin(config[CONF_POWER_PIN]))

    cg.add(
        var.set_change_detection_mode(
            _CHANGE_DETECTION_MODES[config[CONF_CHANGE_DETECTION_MODE]]
        )
    )
    cg.add(var.set_refresh_mode(_REFRESH_MODES[config[CONF_REFRESH_MODE]]))
    cg.add(var.set_clear_color(_CLEAR_COLORS[config[CONF_CLEAR_COLOR]]))
    cg.add(var.set_auto_sleep(config[CONF_AUTO_SLEEP]))
    cg.add(var.set_power_off_after_sleep(config[CONF_POWER_OFF_AFTER_SLEEP]))

    if CONF_LAMBDA in config:
        lambda_ = await cg.process_lambda(
            config[CONF_LAMBDA], [(display_core.DisplayRef, "it")], return_type=cg.void
        )
        if lambda_ is not None:
            cg.add(var.set_writer(lambda_))
