import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import modbus
from esphome.const import CONF_ID, CONF_PORT

DEPENDENCIES = ["modbus", "socket"]
AUTO_LOAD = ["socket"]

modbus_proxy_ns = cg.esphome_ns.namespace("modbus_proxy")
ModbusProxy = modbus_proxy_ns.class_("ModbusProxy", modbus.ModbusDevice, cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(ModbusProxy),
    cv.GenerateID(modbus.CONF_MODBUS_ID): cv.use_id(modbus.Modbus),
    cv.Optional(CONF_PORT, default=502): cv.port,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    
    parent = await cg.get_variable(config[modbus.CONF_MODBUS_ID])
    cg.add(var.set_parent(parent))
    # Registering device with parent so it's in the devices list
    cg.add(parent.register_device(var))
    
    cg.add(var.set_port(config[CONF_PORT]))
