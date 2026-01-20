import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import modbus, sensor
from esphome.const import CONF_ID, CONF_PORT

DEPENDENCIES = ["modbus", "socket"]
AUTO_LOAD = ["socket", "sensor"]

CONF_CLIENTS_CONNECTED = "clients_connected"
CONF_MESSAGES_HANDLED = "messages_handled"
CONF_ERRORS = "errors"

modbus_proxy_ns = cg.esphome_ns.namespace("modbus_proxy")
ModbusProxy = modbus_proxy_ns.class_("ModbusProxy", modbus.ModbusDevice, cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(ModbusProxy),
    cv.GenerateID(modbus.CONF_MODBUS_ID): cv.use_id(modbus.Modbus),
    cv.Optional(CONF_PORT, default=502): cv.port,
    cv.Optional(CONF_CLIENTS_CONNECTED): sensor.sensor_schema(
        unit_of_measurement="clients",
        accuracy_decimals=0,
    ),
    cv.Optional(CONF_MESSAGES_HANDLED): sensor.sensor_schema(
        unit_of_measurement="msgs",
        accuracy_decimals=0,
    ),
    cv.Optional(CONF_ERRORS): sensor.sensor_schema(
        unit_of_measurement="errs",
        accuracy_decimals=0,
    ),
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    
    parent = await cg.get_variable(config[modbus.CONF_MODBUS_ID])
    cg.add(var.set_parent(parent))
    # Registering device with parent so it's in the devices list
    cg.add(parent.register_device(var))
    
    cg.add(var.set_port(config[CONF_PORT]))

    if CONF_CLIENTS_CONNECTED in config:
        sens = await sensor.new_sensor(config[CONF_CLIENTS_CONNECTED])
        cg.add(var.set_clients_connected_sensor(sens))

    if CONF_MESSAGES_HANDLED in config:
        sens = await sensor.new_sensor(config[CONF_MESSAGES_HANDLED])
        cg.add(var.set_messages_handled_sensor(sens))

    if CONF_ERRORS in config:
        sens = await sensor.new_sensor(config[CONF_ERRORS])
        cg.add(var.set_errors_sensor(sens))
