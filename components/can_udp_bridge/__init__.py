import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import binary_sensor, sensor, text_sensor
from esphome.components.esp32 import include_builtin_idf_component
from esphome.const import (
    CONF_ID,
    CONF_MODE,
    CONF_PORT,
    DEVICE_CLASS_CONNECTIVITY,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
)

CODEOWNERS = ["@nejc"]
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor"]

can_udp_bridge_ns = cg.esphome_ns.namespace("can_udp_bridge")
CanUdpBridge = can_udp_bridge_ns.class_("CanUdpBridge", cg.Component)
BridgeMode = can_udp_bridge_ns.enum("BridgeMode", is_class=True)

CONF_TX_PIN = "tx_pin"
CONF_RX_PIN = "rx_pin"
CONF_BIT_RATE = "bit_rate"
CONF_PEERS = "peers"
CONF_BUFFER_FRAMES = "buffer_frames"
CONF_BUFFER_TIMEOUT = "buffer_timeout"
CONF_PEER_TIMEOUT = "peer_timeout"
CONF_KEEPALIVE_INTERVAL = "keepalive_interval"
CONF_RX_RATE = "rx_rate"
CONF_TX_RATE = "tx_rate"
CONF_BUS_ERRORS = "bus_errors"
CONF_RX_MISSED = "rx_missed"
CONF_TX_FAILED = "tx_failed"
CONF_UDP_LOST = "udp_lost"
CONF_PEER_ALIVE = "peer_alive"
CONF_BUS_STATE = "bus_state"
CONF_LAST_FRAME = "last_frame"


MODES = {
    "listen_only": BridgeMode.LISTEN_ONLY,
    "bridge": BridgeMode.BRIDGE,
}

BIT_RATES = [50000, 100000, 125000, 250000]


def _rate_sensor():
    return sensor.sensor_schema(
        unit_of_measurement="frames/s",
        accuracy_decimals=1,
        state_class=STATE_CLASS_MEASUREMENT,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    )


def _counter_sensor():
    return sensor.sensor_schema(
        accuracy_decimals=0,
        state_class=STATE_CLASS_TOTAL_INCREASING,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    )


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(CanUdpBridge),
        cv.Required(CONF_TX_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_RX_PIN): pins.internal_gpio_input_pin_number,
        cv.Optional(CONF_BIT_RATE, default=100000): cv.one_of(*BIT_RATES, int=True),
        cv.Optional(CONF_MODE, default="bridge"): cv.enum(MODES, lower=True),
        # Full mesh: list every OTHER tunnel node. Frames from the local CAN
        # segment are unicast to all of them, and packets are accepted from any.
        cv.Required(CONF_PEERS): cv.All(
            cv.ensure_list(cv.string_strict), cv.Length(min=1, max=4)
        ),
        cv.Optional(CONF_PORT, default=20000): cv.port,
        # Default 1 = per-frame forwarding, no batching. The Solarfocus
        # HMI<->HC protocol is a tight poll/response loop; a buffered relay
        # passed zero traffic in earlier testing. Only raise this for
        # high-traffic sniffing scenarios.
        cv.Optional(CONF_BUFFER_FRAMES, default=1): cv.int_range(min=1, max=32),
        cv.Optional(
            CONF_BUFFER_TIMEOUT, default="2ms"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(
            CONF_PEER_TIMEOUT, default="5s"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(
            CONF_KEEPALIVE_INTERVAL, default="1s"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_RX_RATE): _rate_sensor(),
        cv.Optional(CONF_TX_RATE): _rate_sensor(),
        cv.Optional(CONF_BUS_ERRORS): _counter_sensor(),
        cv.Optional(CONF_RX_MISSED): _counter_sensor(),
        cv.Optional(CONF_TX_FAILED): _counter_sensor(),
        cv.Optional(CONF_UDP_LOST): _counter_sensor(),
        cv.Optional(CONF_PEER_ALIVE): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_BUS_STATE): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_LAST_FRAME): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    # ESPHome trims unused esp-idf components; the legacy "driver" component
    # provides driver/twai.h (same pattern as the stock esp32_can component)
    include_builtin_idf_component("driver")
    include_builtin_idf_component("esp_driver_twai")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_pins(config[CONF_TX_PIN], config[CONF_RX_PIN]))
    cg.add(var.set_bit_rate(config[CONF_BIT_RATE]))
    cg.add(var.set_mode(config[CONF_MODE]))
    cg.add(var.set_port(config[CONF_PORT]))
    for peer in config[CONF_PEERS]:
        cg.add(var.add_peer(peer))
    cg.add(var.set_buffer_frames(config[CONF_BUFFER_FRAMES]))
    cg.add(var.set_buffer_timeout(config[CONF_BUFFER_TIMEOUT].total_milliseconds))
    cg.add(var.set_peer_timeout(config[CONF_PEER_TIMEOUT].total_milliseconds))
    cg.add(
        var.set_keepalive_interval(config[CONF_KEEPALIVE_INTERVAL].total_milliseconds)
    )

    for key, setter in [
        (CONF_RX_RATE, "set_rx_rate_sensor"),
        (CONF_TX_RATE, "set_tx_rate_sensor"),
        (CONF_BUS_ERRORS, "set_bus_errors_sensor"),
        (CONF_RX_MISSED, "set_rx_missed_sensor"),
        (CONF_TX_FAILED, "set_tx_failed_sensor"),
        (CONF_UDP_LOST, "set_udp_lost_sensor"),
    ]:
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(sens))

    if CONF_PEER_ALIVE in config:
        bsens = await binary_sensor.new_binary_sensor(config[CONF_PEER_ALIVE])
        cg.add(var.set_peer_alive_sensor(bsens))
    if CONF_BUS_STATE in config:
        tsens = await text_sensor.new_text_sensor(config[CONF_BUS_STATE])
        cg.add(var.set_bus_state_sensor(tsens))
    if CONF_LAST_FRAME in config:
        tsens = await text_sensor.new_text_sensor(config[CONF_LAST_FRAME])
        cg.add(var.set_last_frame_sensor(tsens))


