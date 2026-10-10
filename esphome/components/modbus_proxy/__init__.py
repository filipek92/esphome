import esphome.codegen as cg
from esphome.components import modbus, sensor, socket
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_PORT,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
)
from esphome.types import ConfigType

CODEOWNERS = ["@filipek92"]
DEPENDENCIES = ["modbus", "network"]
AUTO_LOAD = ["socket", "sensor"]

CONF_CLIENTS_CONNECTED = "clients_connected"
CONF_MESSAGES_HANDLED = "messages_handled"
CONF_ERRORS = "errors"
CONF_MAX_CLIENTS = "max_clients"
CONF_MAX_QUEUE_SIZE = "max_queue_size"

modbus_proxy_ns = cg.esphome_ns.namespace("modbus_proxy")
ModbusProxy = modbus_proxy_ns.class_(
    "ModbusProxy", cg.Component, modbus.ModbusClientDevice
)


def _consume_proxy_sockets(config: ConfigType) -> ConfigType:
    socket.consume_sockets(config[CONF_MAX_CLIENTS], "modbus_proxy")(config)
    socket.consume_sockets(1, "modbus_proxy", socket.SocketType.TCP_LISTEN)(config)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ModbusProxy),
            cv.GenerateID(modbus.CONF_MODBUS_ID): cv.use_id(modbus.ModbusClient),
            cv.Optional(CONF_PORT, default=502): cv.port,
            cv.Optional(CONF_MAX_CLIENTS, default=2): cv.int_range(min=1, max=8),
            cv.Optional(CONF_MAX_QUEUE_SIZE, default=10): cv.int_range(min=1, max=64),
            cv.Optional(CONF_CLIENTS_CONNECTED): sensor.sensor_schema(
                unit_of_measurement="clients",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_MESSAGES_HANDLED): sensor.sensor_schema(
                unit_of_measurement="msgs",
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
            ),
            cv.Optional(CONF_ERRORS): sensor.sensor_schema(
                unit_of_measurement="errs",
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _consume_proxy_sockets,
)

FINAL_VALIDATE_SCHEMA = modbus.final_validate_modbus_device(
    "modbus_proxy", role="client"
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[modbus.CONF_MODBUS_ID])
    cg.add(var.set_parent(parent))

    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_max_clients(config[CONF_MAX_CLIENTS]))
    cg.add(var.set_request_queue_limit(config[CONF_MAX_QUEUE_SIZE]))

    if CONF_CLIENTS_CONNECTED in config:
        sens = await sensor.new_sensor(config[CONF_CLIENTS_CONNECTED])
        cg.add(var.set_clients_connected_sensor(sens))

    if CONF_MESSAGES_HANDLED in config:
        sens = await sensor.new_sensor(config[CONF_MESSAGES_HANDLED])
        cg.add(var.set_messages_handled_sensor(sens))

    if CONF_ERRORS in config:
        sens = await sensor.new_sensor(config[CONF_ERRORS])
        cg.add(var.set_errors_sensor(sens))
