# ESPHome CAN-over-LAN bridge (+ Solarfocus module emulator)

Two ESPHome external components:

- **`can_udp_bridge`** — a transparent CAN bus tunnel over Ethernet. Two or more
  ESP32 nodes each sit on their own CAN segment and forward every frame to the
  others over UDP, so physically separate segments behave as one bus.
- **`sf_module_emulator`** — pretends to be a Solarfocus heating-circuit
  electronic module on a segment, so a Solarfocus controller adopts and drives
  hardware that does not exist.

Built to link a Solarfocus boiler display to a remote heating-circuit module
across buildings, then extended to emulate an extra module. The bridge itself
is protocol-agnostic and works with any classic CAN bus.

> Reverse-engineered from a specific installation. Not affiliated with or
> endorsed by SOLARFOCUS. Read the safety notes before driving real hardware.

## Why

Lightning. The original installation ran the CAN bus between two buildings on
copper, and a nearby strike only has to find one path to take out everything
attached to it. Over the years this destroyed Solarfocus electronics **more
than eight times** — display, power element, heating-circuit modules — each
one either an evening of fault-finding or an expensive replacement.

A CAN bus is a galvanically continuous wire between buildings, so every
strike gets a free ride along it. Replacing that run with a network hop breaks
the path:

```
before:  building A ══════ copper CAN ══════ building B      one surge path
after:   building A ── CAN ── [ESP32] ── LAN/fibre ── [ESP32] ── CAN ── building B
                                        ↑
                          Ethernet magnetics, or full optical
                          isolation if the hop runs over fibre
```

Each CAN segment now ends at an ESP32 a few metres from the equipment it
serves. Ethernet's transformer coupling already provides isolation, and if the
inter-building hop runs over fibre (media converters at each end) there is no
electrical path between the buildings at all.

That the same hardware could then *pretend* to be a heating-circuit module was
a bonus discovered along the way.

## What works

- Per-frame forwarding, ~2 ms added latency each way
- Full mesh, up to 4 peers per node
- Measured 0.002–0.004 % UDP frame loss over a 9300 s soak (~930k frames per
  direction), zero CAN bus errors
- Self-healing: bus-off recovery, driver reinstall if TWAI wedges, automatic
  reconnect after power or link loss (~5 s)
- Diagnostics in Home Assistant: RX/TX rates, bus errors, missed frames,
  tunnel packet loss, per-peer liveness, bus state, last frame
- Status LEDs on the board itself

## Hardware

Per node:

| Part | Notes |
|---|---|
| WT32-ETH01 (ESP32 + LAN8720) | any wired-Ethernet ESP32 works |
| SN65HVD230 / VP230 breakout | 3.3 V CAN transceiver |
| 5 V supply | |

Wiring (defaults, all configurable):

```
ESP32 GPIO14 ──▶ transceiver D / CTX      (TX)
ESP32 GPIO4  ◀── transceiver R / CRX      (RX)
3V3, GND     ─── transceiver
CANH / CANL  ─── the bus
```

On the WT32-ETH01, GPIO 0/16/18/19/21/22/23/25/26/27 belong to the Ethernet
PHY — do not use them. GPIO32/33 are not exposed on all board revisions,
hence GPIO14/4 as defaults.

**Termination:** 120 Ω at both physical ends of each segment, 60 Ω measured.
A node in the middle of a segment must not be terminated.

## Topology

```
                    ┌────────── LAN (UDP, port 20000) ──────────┐
                    │                  │                        │
              ┌─────┴─────┐      ┌─────┴─────┐            ┌─────┴─────┐
              │  node 1   │      │  node 2   │            │  node 3   │
              └─────┬─────┘      └─────┬─────┘            └─────┬─────┘
                 [120Ω]             [120Ω]                   [120Ω]
                    │                  │                        │
              CAN segment 1      CAN segment 2            CAN segment 3
                    │                  │                        │
                 [120Ω]             [120Ω]                   [120Ω]
              ┌─────┴─────┐      ┌─────┴─────┐            ┌─────┴─────┐
              │  device   │      │  device   │            │  device   │
              └───────────┘      └───────────┘            └───────────┘
```

Each node unicasts every locally received frame to all peers and accepts
frames from any of them.

## Quick start

1. Copy `components/` and `packages/` into your ESPHome config directory.
2. Copy `secrets.yaml.example` to `secrets.yaml` and fill it in.
3. Adapt an example from `example/` per node, listing every *other* node under
   `peers:`.
4. Flash the first firmware over serial (GPIO0 to GND during reset), then OTA.

Minimal config:

```yaml
external_components:
  - source: components/

can_udp_bridge:
  id: can_bridge
  tx_pin: 14
  rx_pin: 4
  bit_rate: 100000
  peers:
    - !secret ip_node2
    - !secret ip_node3
```

## Wire format

Cannelloni v2 compatible, so a Linux host can join the tunnel for debugging:

```bash
cannelloni -I vcan0 -C c -l 20000 -R <node-ip>
candump -td vcan0
```

Header `{version=2, op_code=0, seq_no, count(u16 BE)}`, then per frame
`{can_id(u32 BE, EFF 0x80000000 / RTR 0x40000000), len, data[len]}`.

`tools/can_recorder.py` decodes the same framing straight to a timestamped log
with inter-frame deltas, without needing SocketCAN.

## Design notes

**Per-frame forwarding is mandatory for request/response protocols.** The
default is `buffer_frames: 1`. Batching frames into fewer datagrams looks like
an optimisation and completely breaks a poll/response loop — an earlier
buffered version passed zero traffic.

**The datapath does not run in ESPHome's main loop.** Two FreeRTOS tasks own
it: one drains the TWAI receive queue and sends UDP, the other receives UDP and
transmits CAN. Entity state is published from `loop()` via atomics, because
ESPHome entities are not thread-safe.

**Each node ACKs frames on its own segment.** A remote device that stops
responding looks like an unplugged cable, not a bus error. `peer_alive`
reports the tunnel's health separately, and only reads true when *every* peer
is reachable — a partially connected mesh means some segment is stranded.

**Emulator replies are injected into the tunnel explicitly.** A CAN controller
never receives its own transmissions, so a reply that only went on the local
wire would be invisible to a controller on another segment.

## Solarfocus emulator

```yaml
sf_module_emulator:
  - id: mod3
    can_udp_bridge_id: can_bridge
    address: 3          # 1/2/3 -> heating circuits 3+4 / 5+6 / 7+8
    pump_a:
      name: "HC7 Pump"
    # ... see example/sfcantunnel3.yaml
```

It answers the controller's poll for that address with a module identification,
sensor registers and an echo of the commanded output word. Sensor values are
set at runtime (`id(mod3).set_flow_a(x)`), so they can come from real DS18B20s,
Modbus, or a Home Assistant slider. Outputs the controller commands appear as
binary sensors and can drive relays.

The protocol — addressing, registers, sensor encoding and sentinels, output bit
map, sensor-type configuration, discovery handshake — is written up in
[`docs/solarfocus-protocol-notes.md`](docs/solarfocus-protocol-notes.md),
including the negative results.

Beware three overlapping numbering systems for one module: the rotary **device
address** (3), the **heating circuits** it serves (7 and 8), and what the
controller **labels** its shared functions (4).

## Safety

This can switch 230 V pumps and mixer valves. Before wiring anything real:

- Drive LEDs first and confirm the output bits behave as expected.
- Filter short output pulses. The controller blanks its whole output word for
  about 20 ms every 15 s; driving relays straight from the raw bits makes them
  chatter. The emulator debounces 100 ms internally, but anything you build on
  top should too.
- Populating a sensor slot can *overwrite* a real reading in the controller and
  unlock functions it will then try to control. Leave slots absent unless you
  mean it.
- Decide what should happen when the tunnel drops. The emulator keeps the last
  commanded state; a real installation wants a considered failsafe.

## Status and limitations

Working: the tunnel, the mesh, and heating-circuit module emulation, in daily
use on a three-segment installation.

Not solved:

- **Other module families** (solar, fresh water). The controller probes them on
  separate CAN IDs but rejects a heating-circuit identity, and the correct type
  code is unknown. See the protocol notes for what was ruled out.
- **Loss-of-communication behaviour** of real modules is not fully characterised.
- About 1.3 % of emulator replies arrive up to 16 ms late (median is far below
  1 ms). Harmless for this protocol; would want tightening for a stricter one.

## Licence

[MIT](LICENSE). Chosen for compatibility with ESPHome (whose Python codebase is
GPLv3) and because the bridge half is general-purpose enough that anyone
tunnelling a different CAN bus should be able to lift it freely.
