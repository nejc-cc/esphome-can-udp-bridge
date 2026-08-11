# Reference CAN traces

Captures from a real Solarfocus installation, in Vector ASC format (recorded
with [CANgaroo](https://github.com/Schildkroet/CANgaroo) and a CANable 2.0 at
100 kbit/s, 11-bit IDs). Each one demonstrates a specific finding in
[`../solarfocus-protocol-notes.md`](../solarfocus-protocol-notes.md), so the
decode can be checked independently rather than taken on trust.

The bus carried a boiler display (HMI) plus two heating-circuit modules at
device addresses 1 and 2 — poll IDs `0x1E7` / `0x1E8`, replies `0x221` / `0x222`.

| File | Shows |
|---|---|
| `01-output-bit-map.asc` | Each relay output switched individually from the HMI's output test, in the order pump A, pump B, mixer A open, mixer B open, mixer A close, mixer B close — then the DHW pump on the second module. The output word appears in poll bytes 1-2 and is echoed in the ch5 reply. |
| `02-flow-sensor-mapping.asc` | Terminal X37 warmed by hand first, then X38. Exactly two response words move, in that order, which assigns them: ch2 word2 = X37, ch3 word0 = X38. Every other word sits at the `2700` not-connected sentinel. |
| `03-module-address-3-reference.asc` | A real module rotary-switched to **address 3**, giving the reference identification and register contents for that address: poll `0x1E9`, reply `0x223`, ident `00 E6 4B 31 01 11 00 03`, plus the A0/A1/A2 queries. This is what the emulator reproduces. |
| `04-limiting-thermostat-bits.asc` | The X9 limiting-thermostat bridge pulled out, clearing bit 4 of the ch1 status word (`0x0530` → `0x0520`) while bit 5 stays set. Contact bounce is visible as a 30 ms blip before the final removal. The HMI reacts ~10 s later by driving that circuit's mixer closed. |
| `05-sensor-type-config.asc` | Sensor type switched between KTY81-110 and PT1000 for five inputs in turn. Each change flips exactly one bit in the ch0 frame's bytes 3-4, which is how the HMI tells a module which curve to use. Set = KTY81-110, clear = PT1000. |
| `06-module-power-cycle.asc` | A module unplugged and re-powered. While it is missing the HMI switches to probing ch0 every ~262 ms; on return it re-identifies and resumes normal polling in about 50 ms, with no network scan. The boot transient also exposes the second sentinel, `0xF448` = -300.0 °C = "not yet measured". |

## Reading them

`tools/can_recorder.py` in this repo decodes the same protocol live from the
UDP tunnel. For the files here, any CAN analyser that reads Vector ASC will do,
or parse them directly — the format is one frame per line:

```
   0.000000 1  1e8  Rx  d 8  03 50 00 00 00 00 00 00   ...
   ^time     ^ch ^ID          ^8 data bytes
```

A short Python parser is all it takes:

```python
import re
pat = re.compile(r"^\s*(\d+\.\d+)\s+1\s+([0-9a-fA-F]+)\s+Rx\s+d\s+(\d)\s+((?:[0-9A-F]{2}\s+)*[0-9A-F]{2})")
for line in open("01-output-bit-map.asc", encoding="utf-8", errors="replace"):
    m = pat.match(line)
    if m:
        t, can_id = float(m.group(1)), int(m.group(2), 16)
        data = [int(x, 16) for x in m.group(4).split()]
```
