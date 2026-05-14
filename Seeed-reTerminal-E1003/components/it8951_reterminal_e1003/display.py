import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import display
from esphome.const import CONF_ID, CONF_LAMBDA

it8951_reterminal_e1003_ns = cg.esphome_ns.namespace("it8951_reterminal_e1003")
IT8951ReTerminalE1003Display = it8951_reterminal_e1003_ns.class_(
    "IT8951ReTerminalE1003Display", display.DisplayBuffer
)

CONFIG_SCHEMA = (
    display.FULL_DISPLAY_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(IT8951ReTerminalE1003Display),
            cv.Optional("vcom", default=1400): cv.int_range(min=0, max=5000),
        }
    )
    .extend(cv.polling_component_schema("1s"))
)


async def to_code(config):
    cg.add_library("SPI", None)
    var = cg.new_Pvariable(config[CONF_ID])
    await display.register_display(var, config)
    cg.add(var.set_vcom(config["vcom"]))
    if CONF_LAMBDA in config:
        lambda_ = await cg.process_lambda(
            config[CONF_LAMBDA], [(display.DisplayRef, "it")], return_type=cg.void
        )
        cg.add(var.set_writer(lambda_))
