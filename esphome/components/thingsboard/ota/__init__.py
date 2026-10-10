import esphome.codegen as cg
from esphome.components.http_request import CONF_HTTP_REQUEST_ID, HttpRequestComponent
from esphome.components.ota import BASE_OTA_SCHEMA, OTAComponent, ota_to_code
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import coroutine_with_priority
from esphome.coroutine import CoroPriority

from .. import ThingsBoardBridge, thingsboard_ns

CONF_THINGSBOARD_ID = "thingsboard_id"

CODEOWNERS = ["@filimonic"]

AUTO_LOAD = ["md5"]
DEPENDENCIES = ["network", "thingsboard", "http_request"]

OtaThingsBoardComponent = thingsboard_ns.class_("OtaThingsBoardComponent", OTAComponent)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(OtaThingsBoardComponent),
            cv.GenerateID(CONF_THINGSBOARD_ID): cv.use_id(ThingsBoardBridge),
            cv.GenerateID(CONF_HTTP_REQUEST_ID): cv.use_id(HttpRequestComponent),
        }
    )
    .extend(BASE_OTA_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    cv.require_framework_version(
        esp8266_arduino=cv.Version(2, 5, 1),
        esp32_arduino=cv.Version(0, 0, 0),
        esp_idf=cv.Version(0, 0, 0),
        rp2040_arduino=cv.Version(0, 0, 0),
    ),
)


@coroutine_with_priority(CoroPriority.OTA_UPDATES)
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await ota_to_code(var, config)
    await cg.register_component(var, config)

    bridge = await cg.get_variable(config[CONF_THINGSBOARD_ID])
    cg.add(var.set_bridge(bridge))

    await cg.register_parented(var, config[CONF_HTTP_REQUEST_ID])
