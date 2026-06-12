import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID
from . import IPCameraViewer, ip_camera_viewer_ns

DEPENDENCIES = ["ip_camera_viewer"]

IPCameraViewerSwitch = ip_camera_viewer_ns.class_(
    "IPCameraViewerSwitch", switch.Switch, cg.Component
)

CONFIG_SCHEMA = switch.switch_schema(IPCameraViewerSwitch).extend(
    {
        cv.GenerateID(): cv.declare_id(IPCameraViewerSwitch),
        cv.Required("camera_id"): cv.use_id(IPCameraViewer),
    }
)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)

    camera = await cg.get_variable(config["camera_id"])
    cg.add(var.set_camera(camera))
