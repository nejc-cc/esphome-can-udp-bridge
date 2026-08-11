import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.components.can_udp_bridge import CanUdpBridge
from esphome.const import CONF_ADDRESS, CONF_ID

CODEOWNERS = ["@nejc-cc"]
DEPENDENCIES = ["can_udp_bridge"]
AUTO_LOAD = ["binary_sensor"]

sf_module_emulator_ns = cg.esphome_ns.namespace("sf_module_emulator")
SfModuleEmulator = sf_module_emulator_ns.class_("SfModuleEmulator", cg.Component)

CONF_CAN_UDP_BRIDGE_ID = "can_udp_bridge_id"
CONF_POLL_ID = "poll_id"
CONF_REPLY_ID = "reply_id"
CONF_TYPE_CODE = "type_code"
CONF_MODULE_TYPE = "module_type"

# Order must match the SfOutput enum in sf_module_emulator.h
OUTPUTS = [
    "pump_a",
    "pump_b",
    "mixer_a_open",
    "mixer_a_close",
    "mixer_b_open",
    "mixer_b_close",
    "dhw_pump",
    "circulation",
]

_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SfModuleEmulator),
        cv.GenerateID(CONF_CAN_UDP_BRIDGE_ID): cv.use_id(CanUdpBridge),
        # 1/2/3 -> heating circuits 3+4 / 5+6 / 7+8
        cv.Optional(CONF_ADDRESS, default=3): cv.int_range(min=1, max=4),
        # Overrides for probing other module families. Reply ID follows
        # poll ID + 0x3A on every heating-circuit address observed so far.
        cv.Optional(CONF_POLL_ID): cv.hex_int_range(min=0, max=0x7FF),
        cv.Optional(CONF_REPLY_ID): cv.hex_int_range(min=0, max=0x7FF),
        # Identification fields; defaults are the heating-circuit values.
        cv.Optional(CONF_TYPE_CODE, default=0x4BE6): cv.hex_int_range(min=0, max=0xFFFF),
        cv.Optional(CONF_MODULE_TYPE, default=0): cv.hex_int_range(min=0, max=0xFF),
        **{
            cv.Optional(name): binary_sensor.binary_sensor_schema()
            for name in OUTPUTS
        },
    }
).extend(cv.COMPONENT_SCHEMA)

CONFIG_SCHEMA = cv.ensure_list(_SCHEMA)


async def to_code(config):
    for conf in config:
        var = cg.new_Pvariable(conf[CONF_ID])
        await cg.register_component(var, conf)

        bridge = await cg.get_variable(conf[CONF_CAN_UDP_BRIDGE_ID])
        cg.add(var.set_bridge(bridge))
        cg.add(var.set_address(conf[CONF_ADDRESS]))
        if CONF_POLL_ID in conf or CONF_REPLY_ID in conf:
            poll = conf.get(CONF_POLL_ID, 0x1E6 + conf[CONF_ADDRESS])
            cg.add(var.set_ids(poll, conf.get(CONF_REPLY_ID, poll + 0x3A)))
        cg.add(var.set_ident(conf[CONF_TYPE_CODE], conf[CONF_MODULE_TYPE]))

        for idx, name in enumerate(OUTPUTS):
            if name in conf:
                bs = await binary_sensor.new_binary_sensor(conf[name])
                cg.add(var.set_output_sensor(idx, bs))
