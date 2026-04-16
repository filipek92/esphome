import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID, CONF_PORT, CONF_LEVEL, CONF_TRIGGER_ID
from esphome.core import CORE

CONF_SERVER = "server"
CONF_TOKEN = "token"
CONF_LOG_LEVEL = "log_level"
CONF_ON_RPC = "on_rpc"
CONF_ON_ATTRIBUTE = "on_attribute"

DEPENDENCIES = ["json", "logger", "network"]

thingsboard_ns = cg.esphome_ns.namespace('thingsboard')
ThingsBoardBridge = thingsboard_ns.class_('ThingsBoardBridge', cg.Component)
SendTelemetryAction = thingsboard_ns.class_('SendTelemetryAction', automation.Action)
SendAttributeAction = thingsboard_ns.class_('SendAttributeAction', automation.Action)

CONF_KEY = "key"
CONF_VALUE = "value"

LOG_LEVELS = {
    "NONE": 0,
    "ERROR": 1,
    "WARN": 2,
    "INFO": 3,
    "CONFIG": 4,
    "DEBUG": 5,
    "VERBOSE": 6,
    "VERY_VERBOSE": 7,
}

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(ThingsBoardBridge),
    cv.Required(CONF_SERVER): cv.string,
    cv.Required(CONF_TOKEN): cv.string,
    cv.Optional(CONF_PORT, default=8883): cv.port,
    cv.Optional(CONF_LOG_LEVEL): cv.one_of(*LOG_LEVELS, upper=True),
    cv.Optional(CONF_ON_RPC): automation.validate_automation(single=True),
    cv.Optional(CONF_ON_ATTRIBUTE): automation.validate_automation(single=True),
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID], config[CONF_SERVER], config[CONF_PORT], config[CONF_TOKEN])
    await cg.register_component(var, config)

    if CONF_LOG_LEVEL in config:
        cg.add(var.set_log_level(LOG_LEVELS[config[CONF_LOG_LEVEL]]))

    if CONF_ON_RPC in config:
        await automation.build_automation(
            var.get_rpc_trigger(),
            [(cg.std_string, "method"), (cg.std_string, "params")],
            config[CONF_ON_RPC],
        )

    if CONF_ON_ATTRIBUTE in config:
        await automation.build_automation(
            var.get_attribute_trigger(),
            [(cg.std_string, "key"), (cg.std_string, "value")],
            config[CONF_ON_ATTRIBUTE],
        )

    # Vždy registrovat log listener (shared atribut může zapnout logování za běhu)
    from esphome.components.logger import request_log_listener
    request_log_listener()

    if CORE.is_esp32:
        from esphome.components.esp32 import add_idf_component
        add_idf_component(name="espressif/mqtt", ref="1.0.0")

    if CORE.using_arduino:
        cg.add_library("knolleary/PubSubClient", "2.8")


SEND_TELEMETRY_ACTION_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.use_id(ThingsBoardBridge),
    cv.Required(CONF_KEY): cv.templatable(cv.string),
    cv.Required(CONF_VALUE): cv.templatable(cv.string),
})

SEND_ATTRIBUTE_ACTION_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.use_id(ThingsBoardBridge),
    cv.Required(CONF_KEY): cv.templatable(cv.string),
    cv.Required(CONF_VALUE): cv.templatable(cv.string),
})


@automation.register_action(
    "thingsboard.send_telemetry", SendTelemetryAction, SEND_TELEMETRY_ACTION_SCHEMA,
    synchronous=True,
)
async def send_telemetry_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    templ = await cg.templatable(config[CONF_KEY], args, cg.std_string)
    cg.add(var.set_key(templ))
    templ = await cg.templatable(config[CONF_VALUE], args, cg.std_string)
    cg.add(var.set_value(templ))
    return var


@automation.register_action(
    "thingsboard.send_attribute", SendAttributeAction, SEND_ATTRIBUTE_ACTION_SCHEMA,
    synchronous=True,
)
async def send_attribute_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    templ = await cg.templatable(config[CONF_KEY], args, cg.std_string)
    cg.add(var.set_key(templ))
    templ = await cg.templatable(config[CONF_VALUE], args, cg.std_string)
    cg.add(var.set_value(templ))
    return var
