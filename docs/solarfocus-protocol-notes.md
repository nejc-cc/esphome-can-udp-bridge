# Solarfocus HMI ↔ HC module CAN protocol notes

Sources: CANgaroo traces (`docs/traces/*.asc`), 100 kbps, 11-bit IDs
+ `Upgrade-Package-for-heating-circuit_Installation-Manual.pdf` (official pinout).
Latest/best trace: "HMI searching +2 HC modules connected to network.asc" (40011 frames, 138 s,
both modules present, outputs toggled one at a time near the end).

## Addressing (CONFIRMED)
- HMI poll ID = 0x1E6 + module_address; module response ID = 0x220 + module_address
  - addr1 → poll 0x1E7 / resp 0x221 · addr2 → 0x1E8 / 0x222 · addr3 → **0x1E9 / 0x223**
- Response last byte (byte7) = module address echo.
- Module address is set by a **rotary switch** on the module, valid 1..3 for HC extension modules.
- Address → heating circuits (from manual): addr1 = HC 3/4, addr2 = HC 5/6, **addr3 = HC 7/8**
  (circuits 1/2 live on the boiler power element / HMI itself).
- 0x1E9 is probed every ~709 ms with the ch0 frame and currently unanswered → free slot.

## Frame format (CONFIRMED)
- Poll (HMI→module): `[ch, out_lo, out_hi, 0,0,0,0,0]`
  - `ch` = register/channel selector, cycles 0x01..0x05 (~15.2 ms per poll, ~76 ms full cycle)
  - `out_lo`/`out_hi` = **relay output command word**, sent in EVERY poll regardless of channel
- Response (module→HMI): `[ch, d0,d1,d2,d3,d4,d5, addr]` — echoes `ch`, 6 data bytes, address trailer
- Verified over 11487 poll/response pairs: channel always echoes, zero mismatches.
- ch5 response bytes[3..4] **echo the commanded output word back** (verified 100%, ~2300 pairs).
- Response latency: immediate (below CANgaroo's timestamp resolution, ≪1 ms).

## Data encoding
- Temperatures: little-endian u16, **×0.1 °C** (e.g. `E5 00`=229→22.9 °C, `64 01`=356→35.6 °C)
- **`8C 0A` (=2700) is the "sensor not connected" sentinel** — appears wherever no sensor is wired.
  Important: an emulated module must NOT report 2700 for a sensor it wants recognised.
- Observed steady payloads, module addr1 (`8C 0A` = absent sensors):
  - ch1 `30 05 00 00 00 00` · ch2 `8C 0A 8C 0A 64 01` · ch3 `64 01 8C 0A 00 00`
  - ch4 `8C 0A 8C 0A 8C 0A` · ch5 `8C 0A <out_lo> <out_hi> 04 04`
- Module addr2: ch1 `00 05 …` · ch2 `8C 0A 8C 0A 70 01` · ch3 `6E 01 F2 00 00 00`
  · ch4 `8C 0A 8C 0A 0F 01` · ch5 `E5 00 <out_lo> <out_hi> 03 03`
- ch5 trailing pair differs per module (addr1 `04 04`, addr2 `03 03`) — meaning unknown
  (module type? config? circuit count?).

## Identification / discovery handshake (CONFIRMED sequence)
1. HMI ch0 probe → `[00, 00 00 3C 0E 00 32 32]` on 0x1E6+addr (every ~709 ms while scanning)
2. Module answers on 0x220+addr: `[00, E6 4B, 31 01, XX, 00, addr]`
   - addr1: `00 E6 4B 31 01 11 00 01` · addr2: `00 E6 4B 31 01 14 00 02`
   - `E6 4B`(=0x4BE6) and `31 01`(=0x0131=305, version 3.05?) identical on both modules
   - **byte5 differs: 0x11 vs 0x14 — UNKNOWN meaning** (module type / fitted-peripheral bitmask?)
3. HMI then queries `A0`, `A1`, `A2` (payload all zero, ~0.5 s apart)
   - module answers `A0 03 00 00 00 00 00 <addr>`, `A1 00 …`, `A2 00 …`
4. Then normal ch1..5 polling begins.
- Network scan is technician-initiated (service menu code), runs **60 s**, then offers
  "Apply + default parameters" (unlocks components AND resets related params to factory) or
  "Adopt" (unlocks, keeps params). Scan also detects sensor types (PT1000 vs KTY).
- Other probe sweeps seen during scan (never answered here): 0x191–0x199 (@277 ms),
  0x19A–0x19E, 0x1B7, 0x1F4/0x1F5, 0x770, 0x7B9–0x7C0, 0x090 (@~709 ms) — other module families.

## Relay outputs
Physical pinout per module (manual §4.1.1) — for **addr3** the "/7/8" variants apply:
| Terminal | Function (addr1 / addr2 / addr3) |
|---|---|
| X7 | Buffer charge pump 2/3/4 |
| X8 | DHW tank charge pump 2/3/4 |
| X9 | **Heating circuit pump 3/5/7** |
| X10 | **Heating circuit pump 4/6/8** |
| X11 | **Mixer Closed/Open 3/5/7** |
| X12 | **Mixer Closed/Open 4/6/8** |
| X36/X44 | Lower/Upper buffer sensor 2/3/4 |
| X37 | **HC flow sensor 4/6/8** |
| X38 | **HC flow sensor 3/5/7** |
| X39 | DHW tank sensor 2/3/4 |
| X40/X41 | Room temp controller 4/6/8 and 3/5/7 (optional) |
X9/X10 are 230 V relay outputs with a limiting-thermostat loop (pins 4-5 must be linked if unused).
Fuse F3 6.3 AT protects relay outputs.

### Output bit map (SOLVED — trace "outputs on 0x221 and DHW output on 0x222.asc")
Each output was toggled individually from the HMI output test, in this order:
pump A, pump B, mixer A open, mixer B open, mixer A close, mixer B close (identical sequence
also present in the big 2-module trace, so it is reproducible).

| Bit | Mask (lo,hi) | Terminal | Function (addr1 / addr2 / **addr3**) |
|---|---|---|---|
| lo2 | `04 00` | X8  | DHW tank charge pump 2/3/4 |
| lo3 | `08 00` | X11 | Mixer **OPEN**  3/5/**7** |
| lo4 | `10 00` | X11 | Mixer **CLOSE** 3/5/**7** |
| lo5 | `20 00` | X12 | Mixer **OPEN**  4/6/**8** |
| lo6 | `40 00` | X12 | Mixer **CLOSE** 4/6/**8** |
| hi2 | `00 04` | X9  | Heating circuit pump 3/5/**7** |
| hi3 | `00 08` | X10 | Heating circuit pump 4/6/**8** |

Cross-check that fixes the mixer pairing: idle state in the big trace was `out_lo = 0x50`
(= lo4|lo6) = *both mixers driven closed* — sensible for an idle system. The alternative
pairing would make 0x50 mean "mixer 4 open AND closed simultaneously", which is nonsense.
Unassigned bits (lo0, lo1, lo7, hi0, hi1, hi4+) — X7 buffer charge pump presumably among them.

### Sensor register map (PARTLY SOLVED — trace "X37 and X38 being heated by hand … 0x221.asc")
HC1/addr1 has ONLY the two HC flow sensors wired (X37, X38); every other word reads the
2700 "not connected" sentinel. After warming X37 first and then X38, the two moving words
rose in that order, giving an unambiguous assignment:

| Register | Bytes in response | Terminal | Function (addr3) |
|---|---|---|---|
| **ch2 word2** | d4,d5 | X37 | **HC flow sensor 4/6/8 → flow sensor HC8** |
| **ch3 word0** | d0,d1 | X38 | **HC flow sensor 3/5/7 → flow sensor HC7** |

### Remaining sensor slots (SOLVED 2026-07-29 by emulator probe)
Each slot was set to a distinct value on the emulated addr-3 module and read off the HMI
(HMI numbers an addr-3 module's functions as "4"):

| Register | Value set | HMI shows |
|---|---|---|
| ch2 word0 | 30 | **buffer 4 middle** (displayed in the buffer *1* middle slot — see below) |
| ch2 word1 | 31 | **buffer 4 bottom** (X36, HMI input I6) |
| ch3 word1 | 32 | **DHW tank 4** (X39, HMI input I7) |
| ch4 word0 | 33 | **circulation** |
| ch4 word1 | 34 | **buffer 4 top** (X44, HMI input I5) |
| ch4 word2 | 35 | nothing |
| ch5 word0 | 36 | nothing |

Reporting these **unlocked buffer tank 4, DHW tank 4 and the recirculation module** in the HMI.
(Earlier guess that ch4.w2/ch5.w0 were buffer/DHW was wrong — they were live on the reference
module but map to nothing the HMI displays for an addr-3 module.)
HMI input numbering: I3 = X38, I4 = X37, I5 = X44, I6 = X36, I7 = X39.

### Circulation output bit (NEW)
With the recirculation module unlocked, ch5's echo showed the output word toggling `0x0001`
→ `0x0000` when circulation was switched off. **out_lo bit0 = circulation pump.**

### ch1 word0 = status flags — bits 4,5 are the LIMITING THERMOSTAT inputs (SOLVED)
Observed: 0x221 = `0x0530`, 0x222 = `0x0500`, 0x223 (=HC1 moved to addr3) = `0x0530`.
After confirming HC1's limiting-thermostat inputs are **closed** (circuits 3/4) while HC2's
are **open** (circuits 5/6) — the difference is exactly bits 4 and 5 (`0x30`).
→ **bit4 = limiting thermostat closed, circuit A (3/5/7); bit5 = circuit B (4/6/8)**
Manual §4.1.4: X9/X10 pins 4-5 must be linked if no thermostat is used. An open input almost
certainly blocks the circuit from running — the emulator must report both bits SET.
Bits 8 and 10 (`0x0500`) are set on every module in every trace — meaning unknown (capability
flags?). ch5 word2 (addr1 `0x0404`, addr2 `0x0303`) still unknown, follows the module.

### Version encoding in the ident frame (SOLVED)
HC1 reports HW rev **1.10**, SW **1.31** in its menus; its CAN ident is `00 E6 4B 31 01 11 00 03`:
- bytes3,4 = `31 01` → BCD → **SW version 1.31**
- byte5 = `11` → BCD → **HW revision 1.1(0)**  (HC2's `0x14` = HW rev 1.4 — different board)
- bytes1,2 = `E6 4B` (0x4BE6) identical on both modules → module type / article code
So none of it is address-derived; emulate HC1's values verbatim.

### ch0 bytes 3-4 = SENSOR TYPE CONFIG BITMASK (SOLVED 2026-07-29)
Changing an input's sensor type in the HMI toggles exactly one bit in the ch0 frame's
LE u16 at bytes 3-4. Baseline `0x0E3C`; each change flipped one bit and flipping it back
restored `0x0E3C`:

| Bit | HMI input | Terminal |
|---|---|---|
| bit3 | I6 | X36 buffer lower |
| bit4 | I4 | X37 HC flow B (circuit 8) |
| bit5 | I3 | X38 HC flow A (circuit 7) |
| bit6 | I7 | X39 DHW tank |
| bit9 | I5 | X44 buffer upper |

**Polarity: bit set = KTY81-110, bit clear = PT1000.** Confirmed by I7, which started as
PT1000 (bit6 clear in baseline) and set the bit when switched to KTY — opposite direction
to the other four, which started as KTY.
Bits 2, 10, 11 are set in baseline and unexplained (other inputs?); bits 0,1,7,8,12-15 clear.

Consequence: **the module does the ADC→temperature conversion**, the HMI just tells it which
curve to use. Changing the type for the emulated module changes nothing on screen (it sends
finished temperatures), whereas on a real module the displayed value jumps (e.g. 36 → 23 °C).
The emulator can safely ignore this field. It is also the best remaining candidate location
for the emergency/boiler-state flag — worth re-diffing during a real burn.

### Poll padding — CORRECTED
For **ch1..ch5 polls**, bytes 3..7 are always zero: the HMI tells the module only
`channel` + `output word`.
**But the ch0 (ident-probe) frame carries live data** and is periodically sent even to modules
that are present: `[00, 00, 00, d3, d4, 00, 32, 32]` with d3/d4 varying —
`0x0E3C`(=3644) in four traces, `0x0E0C`(=3596) in the power-cycle trace. Trailing `32 32`
(=50,50) constant. So the HMI *does* broadcast something (boiler temp? setpoint? state) here.
→ This is now the prime suspect for the emergency/enable flag hunt, alongside the unassigned
output-word bits. Diff the ch0 payload between "boiler idle" and "boiler firing".

## GROUND TRUTH for address 3 (trace "HC1 temporarily set to act as HC3…")
Real module HC1 rotary-switched to address 3. Everything the emulator must reproduce:
```
ident   0x1E9 [00 00 00 3C 0E 00 32 32]  ->  0x223 [00 E6 4B 31 01 11 00 03]
A0      0x1E9 [A0 00 …]                  ->  0x223 [A0 03 00 00 00 00 00 03]
A1      0x1E9 [A1 00 …]                  ->  0x223 [A1 00 00 00 00 00 00 03]
A2      0x1E9 [A2 00 …]                  ->  0x223 [A2 00 00 01 00 00 00 03]   (byte3=01 here)
ch1     -> [01 30 05 00 00 00 00 03]
ch2     -> [02 8C 0A 8C 0A <X37 lo hi> 03]      X37 = flow sensor HC8
ch3     -> [03 <X38 lo hi> 8C 0A 00 00 03]      X38 = flow sensor HC7
ch4     -> [04 8C 0A 8C 0A 8C 0A 03]            (all sensors absent)
ch5     -> [05 8C 0A <out_lo> <out_hi> 04 04 03]
```
**Key finding: ident byte5 (0x11) and the ch1/ch5 constants followed the MODULE, not the address**
— HC1 reported 0x11 / 0x0530 / 0x0404 both as addr1 and as addr3. So those bytes are module
identity/revision, not address-derived, and the emulator can simply use HC1's values.
Output bit map on addr3 verified identical to addr1 (same toggle sequence, same bits).

## Boiler ON/OFF (trace "HMI set from OFF to boiler ON state and OFF again")
**Null result, and an important one:** switching the boiler on and off changed *nothing* in the
HC module conversation — output words stayed put (addr1 all-off, addr2 0x50), payloads only
jittered by 0.1 °C. The capture contains ONLY the four poll/response IDs; no boiler-state
broadcast exists on this bus in normal operation (the 0x090/0x19x/0x7Bx sweeps appear only
during a network scan). HC modules are pure slaves and are not told about boiler state.
→ Consequence for the emergency logic: there is no "boiler on/off" flag to latch onto. Use the
module's own last commanded output state as the proxy — if pumps/mixers were being driven when
comms dropped, the circuit was active (run emergency); if everything was off, stay off. This is
locally available, needs no protocol support, and matches the intent exactly.

## Live circuit control (trace "heating circuit 3 auto mode simulation on bench")
Circuit 3 running, mixer travel time configured 120 s, flow setpoint 26 °C, with the flow sensor
hand-heated and allowed to overshoot.
- Pump 3 (`hi2`) ON for the whole active period; when the circuit was switched off the pump
  dropped and MIX3-CLOSE (`lo4`) went on continuously (HMI drives closed >2 min to reach the
  end stop; mixer motors have limit switches so this is harmless).
- **Mixer control is 3-point pulsed**: an isolated 3.10 s CLOSE pulse at t=11.4 s (≈2.6 % of the
  120 s travel) to correct the overshoot. Expect short pulses, not continuous drive.
- Independently re-confirms the sensor map: circuit 3's flow reading is **ch3 word0 (X38)** —
  it sat at 29.7 °C vs the 26 °C setpoint, which is exactly why the HMI was closing.

### ⚠ 15-second output blanking (important for the emulator's relay driver)
The HMI blanks the **entire output word to 0x0000 for ~20 ms every 15.00 s**, then restores it.
Seen on the pump while running and on the close relay while parked (t=3.92, 18.93, 33.93, 48.93,
63.95, 78.97, 93.97 — spacing 15.00±0.01 s). Presumably a watchdog/refresh handshake.
→ The emulator must **filter output pulses shorter than ~100 ms** before driving physical relays,
otherwise every relay chatters every 15 s. (Real modules evidently ride through it.)

## Limiting thermostat bits (CONFIRMED by direct experiment)
Trace "removing HC1 limiting thermostat bridge on X9 on 0x221": pulling the X9 bridge cleared
**bit4** of ch1 word0 (0x0530 → 0x0520) while bit5 stayed set; a 30 ms blip at t=15.0 shows the
contact bouncing before final removal at t=18.8.
→ **bit4 = X9 thermostat (circuit A, 3/5/7), bit5 = X10 thermostat (circuit B, 4/6/8)**, closed=1.
The HMI reacted ~10.7 s later by driving MIX3-CLOSE — i.e. an open thermostat makes the HMI
close that circuit's mixer. The emulator must report both bits set for circuits 7/8 to run.

## Module power-loss / recovery (trace "unplugging HC1 from power … re-powering it")
- Module silent for 26.8 s. While it is missing the HMI switches from ch1..5 polling to the
  **ch0 ident probe every ~262 ms** — that is the "where did you go" state.
- **No network scan is needed to get an enrolled module back.** On return the sequence is:
  ident reply → immediate ch1..5 poll round → A0/A1/A2 → normal cycling. Total re-entry ≈ 50 ms.
  The emulator can therefore reboot freely; it just has to answer the ch0 probe.
- **Fresh-boot values reveal a second sentinel and signedness:** immediately after power-up the
  module reports `48 F4` = **-3000 = -300.0 °C = "not yet measured / invalid"**, and values like
  `69 FE` = -407 = -40.7 °C. So readings are **signed int16 ×0.1 °C** with two sentinels:
  `0x0A8C` (+2700) = sensor open/not connected, `0xF448` (-3000) = invalid/uninitialised.
  Real readings populate progressively over the first seconds after boot.
  (This also explains the odd rare payloads seen in earlier traces — they were boot transients.)

## Addressing beyond 3, and other module families (analysed 2026-07-29)
- **Addresses 4+ are never polled.** Across every trace the HMI polls only 0x1E7/0x1E8/0x1E9
  (addresses 1-3), matching the manual's "maximum of 3 extension packages". Setting a module's
  rotary switch to 4 will simply make it invisible — no point trying.
- **Reply ID = poll ID + 0x3A**, verified for all three heating-circuit addresses.
- To emulate other families you must answer their own probe IDs. Unanswered probes seen, with
  the reply ID the +0x3A rule predicts:

| Probe | cadence | payload | predicted reply |
|---|---|---|---|
| 0x1B7 | ~709 ms | `00 00 03 FF 0E 00 32 32` | 0x1F1 |
| 0x191-0x194 | ~277 ms | `00 00 03 00 …` | 0x1CB-0x1CE |
| 0x195-0x198 | ~277 ms | `00 00 02 00 …` | 0x1CF-0x1D2 |
| 0x199, 0x19A-0x19E | 277/709 ms | all zero | 0x1D3-0x1D8 |
| 0x1F4, 0x1F5 | ~693 ms | all zero | 0x22E, 0x22F |
| 0x770 | ~277 ms | all zero | 0x7AA |
| 0x7B9-0x7C0 | ~693 ms | all zero | 0x7F3-0x7FA |
| 0x090 | ~693 ms | all zero | 0x0CA |

**Best candidate for the SOLAR family: 0x191-0x194.** The solar module manual (§5.1.9) says
solar modules use device **address 1 to 4** — and 0x191-0x194 is exactly a 4-ID block, polled
at the same fast ~277 ms cadence as the heating-circuit modules, with a constant byte2 = 0x03
that looks like a family/type code. 0x195-0x198 is a second 4-ID block with byte2 = 0x02 —
plausibly the fresh-water modules. (0x1B7 is a single ID whose payload merely has the same
config-frame *shape*; it can't be a 4-address family.)
The component accepts `poll_id:`/`reply_id:` overrides in `emulate_module:` for probing these.

### Solar control module (SCM) pinout — manual §5.1.1
| Terminal | Function |
|---|---|
| o1 | Solar pump 1 (HE pump) — 230 V relay |
| o2 | Solar pump 2 or switching valve — 230 V relay |
| OUT1 / OUT2 | HE-pump speed control signal for pump 1 / 2 |
| i1 | Collector sensor 1 (PT1000) |
| i2 | Collector sensor 2 (PT1000) |
| i3 | Collector **return** temperature sensor (PT1000) |
| i4 | Tank sensor 2 |
| i5 | Collector **flow** temperature sensor (PT1000) |
| i9 | Tank sensor 1 |
| i15 | Flow volume sensor for heat meter (**pulse** input, 24 V + I) |

Up to 3 solar cycles per module; max 2 modules on a boiler control / central control.
HE-pump speed signal is PWM (90-5000 Hz, 1000 Hz nominal, 4-24.5 V) **or** 0-10 V — module
generates it, so an emulator only needs to accept whatever speed value the HMI sends over CAN
(expect a percentage field somewhere in the register map, not just an on/off bit).
i15 is a pulse input the module integrates itself — an emulator reports flow/energy values,
never pulses.

## Hardware: HC module PCB has more than the manual documents
Unpopulated on the real HC1 board: **X13 (mixer output, with two relay footprints nearby)**,
X28 (spare relay output), and inputs X51, X58, X59, X60, X35, X43 (spare), plus unsoldered
X31, X32, X42, X56, X57. A third mixer's worth of relays and many more inputs than a heating
circuit module needs — consistent with one PCB/firmware serving several module families
(solar control, fresh water, ...). The unassigned output bits (lo1, lo7, hi0, hi1, hi4-7) are
the likely homes for X13-open/close and X28.

## ⚠ Buffer middle slot displaces another buffer's reading (HMI indexing bug)
Populating ch2 word0 on an addr-3 module makes that value appear in the HMI's **buffer 1
middle** field. The value itself is reported correctly — the fault is in the HMI: buffer 1 has
no middle sensor fitted in this setup, and rather than leaving the field empty the HMI falls
back to the next available buffer-middle sensor it can find and shows that instead. So a
genuine reading from one buffer is displayed against a different buffer.

Consequences: leave the slot at the 270 absent sentinel unless deliberately testing, and never
populate it on a real installation — anything reading that field, whether a person or a control
decision, would be looking at the wrong tank. Presumably the same substitution happens for other
sensor positions when a lower-numbered tank is incompletely equipped, so this is worth keeping in
mind for any slot, not just this one.

## Solar-family probing attempts (2026-07-29) — NOT yet successful
Emulator pointed at poll 0x191, replying on 0x1CB (poll + 0x3A rule):
1. Reply `00 E6 4B 31 01 11 00 01` (heating-circuit ident): 195 polls, 195 replies, HMI never
   escalated past the ch0 probe. Heard but not accepted.
2. Reply `00 E6 4B 31 01 11 03 01` (byte6 = 0x03, echoing the probe's byte2 family code):
   221 polls, 221 replies, still ch0 only. **byte6 is not the gate.**
Probe cadence on 0x191 is ~320 ms and runs continuously, not only during a network scan.

**Why fuzzing the type code is a bad next step:** two unknowns produce identical symptoms —
the ident content may be wrong, *and* the reply ID may be wrong (poll + 0x3A is inferred from
the heating-circuit family alone and need not generalise). A 16-bit type-code sweep is ~5.8 h
at 320 ms and proves nothing if the reply ID was wrong all along.

3. **Rotary switch does NOT select the module role — DISPROVEN 2026-07-29.** HC1 set to address
   4 answered *nowhere*: the whole 77 s capture contained only 0x222 (real HC2) and 0x1CB (the
   emulator). This was predictable from the manuals alone — heating circuit uses addresses 1-3
   and solar 1-4, so positions 1-3 would be ambiguous if the rotary chose the role. Roles are
   firmware-baked; the shared PCB (third mixer, spare relay, unpopulated inputs) is just cost
   engineering.

**B&R simulator images (TII-touch-Simulation-V26060.zip): readable, but no easy table.**
Correcting an earlier note: the `.br` files are NOT compressed — entropy 5.8-6.4 bits/byte and
thousands of printable string runs. The application is in there (German UI strings: Solar 4293
hits, Heizkreis, Frischwasser, Kollektor, Puffer). However the 0x4BE6 type code appears only at
random-chance frequency (~276 hits in an 18 MB file is exactly 1/65536), and there is no cluster
of similar 16-bit codes anywhere near it — so module type codes are almost certainly immediate
operands in compiled code, not a data table. Extracting them means disassembling the USERROM
images (B&R Automation Runtime), i.e. a real RE project, not a search.

**Remaining realistic options for solar emulation:**
(a) obtain a used solar module and capture its ident — one 5-minute capture yields both the
    real reply ID and the real ident, exactly as the address-3 capture did for HC3;
(b) disassemble the USERROM images. Option (a) is very likely cheaper.

## Five-hour capture during a real burn (2026-08-19, screed-drying programme)

17907 s / 3.55 M frames, recorded through the tunnel mirror while the boiler was lit and one
heating circuit ran a screed-drying programme (interrupted and restarted at a higher setpoint).
Only module address 1 was in service, so the trace is a clean picture of one real module.

### The HMI does all the control — the module cannot regulate
The poll frame `[ch, out_lo, out_hi, 0,0,0,0,0]` carries **no setpoint, no target temperature
and no control parameters**, and none appeared in five hours of active regulation. The module is
never told what temperature to aim for; it reports sensors and switches relays. All control law
lives in the HMI, so an emulator never needs to implement one.

### No emergency flag exists in the protocol (NEGATIVE RESULT — hypotheses closed)
Through ignition, five hours of operation and a programme restart:
- the ch0 config word never changed after the initial sensor-type edits (constant `0x0E0C`);
- no new output-word bit ever appeared (only lo2, lo5, lo6, hi3 were ever seen).

Both leading candidates are therefore eliminated. The modules' known emergency behaviour on
loss of communication must be **decided locally by the module from its own last commanded output
state** — active when comms drop means run, idle means stay idle. That is what an emulator should
implement; there is nothing to listen for.

### Sensor-type bitmask confirmed independently
Switching X37 and X38 from KTY81-110 to PT1000 cleared exactly bits 4 and 5
(`0x0E3C` → `0x0E2C` → `0x0E0C`), and X38's reported value jumped 36.1 → 21.9 °C at that moment
— matching the 36 → 23 °C shift seen when the type was toggled on the bench.

### Mixer control law
- The atomic step is a **3.1 s pulse** (dominant width for both directions). At the configured
  120 s full travel that is ~2.6 % of position per pulse.
- The controller varies the **interval between pulses, not their width** — three-point step
  control, not continuous modulation.
- After ~25 minutes of finding the working point, the mixer was almost idle for four hours:
  three isolated pulses while flow held 29.5–30.5 °C. The circulating pump ran 99.5 % of the time.
- The "15 s" drive periods in the pulse statistics are not control action, they are the 15 s
  output-word blanking cutting a continuous command into pieces.

### Open/close never commanded together
Zero simultaneous open+close commands in five hours, on either circuit. Useful to know, but not
a substitute for a hardware interlock: that protects against firmware or emulator faults, which
protocol correctness says nothing about.

## PENDING EXPERIMENT: what actually triggers a module's emergency mode

The modules are known from field experience to run pumps and open mixers when communication is
lost — but only when the system was active, not when it was idle. Since the protocol carries no
boiler state (see the burn capture above), the module must infer this locally. Two candidate
rules make *different* predictions, so the timing of the test decides whether it is informative:

| Rule | Prediction if comms are cut just after a Stop command, while the pump still runs |
|---|---|
| A: module somehow knows boiler state | no emergency (the boiler is stopping) |
| B: module infers from its own last commanded outputs | **emergency** (the pump was running) |

Rule A has no mechanism in the observed protocol, so B is expected — but it has not been observed
directly. Note that after a Stop command the circulating pump was still commanded on for at least
the following 78 s, which is what makes this window discriminating.

Method: mute the tunnel on the module's segment (the "Tunnel Enabled (test)" switch) and watch
the relays. Muting withholds transmit only, so the capture keeps running throughout. Three cases:

1. circuit active, boiler burning — expect emergency; confirms the basic claim
2. just after Stop, pump still running — **the discriminating case**
3. circuit fully off — both rules predict nothing; useful only as a control

Also measure **how long** the module waits before reacting: that timeout is what an emulator has
to reproduce. Do this with someone present at the boiler — while muted, the controller has no
authority over that circuit.

## Open questions / experiments needed
1. ~~**Emergency mode flag**~~ — ANSWERED, see the five-hour burn capture above: no flag
   exists in the protocol; the behaviour is module-local. Original note retained for context:
   **Emergency mode flag.** Field experience indicates that HC modules
   drop into an emergency routine on loss of comms — mixers open, pumps run — to dump heat when the
   boiler loses power, and that this does NOT happen when the circuits were off. Since poll bytes
   3..7 are always zero, any HMI-sent enable flag must be one of the **unassigned output-word
   bits**. PREDICTION TO TEST: with a real fire burning and circuits active, an extra bit should
   appear in the output word that is absent in the bench captures. Capture during real operation
   and diff. (If no new bit appears, the module decides locally from its last commanded state.)
2. **Limiting thermostat bit confirmation:** bridge just ONE of HC2's two inputs and check whether
   bit4 or bit5 appears in ch1 word0 — nails which bit belongs to which circuit. ~1 minute.
3. **Network-scan "Apply" phase.** Capture a full scan + Apply while a module appears, to see
   whether the HMI writes any config/parameters to the module (no write command observed yet).
4. **Full mixer opening sequence** from 0 % up to setpoint — the existing capture only caught the
   closing tail. Shows pulse cadence/duty during normal modulation.
5. **Module unplugged mid-operation** — how fast the HMI notices, what it retries, what it alarms.
   Informs how the emulator should behave when *it* is the one that drops out.
6. Remaining sensor words (ch3.w1, ch4.w2, ch5.w0) if buffer/DHW emulation is ever wanted.

## Tunnel performance context
- 9300 s production soak through can_udp_bridge: 22 + 41 lost UDP packets of ~930k/direction
  (0.002–0.004%) = one lost 15 ms poll per ~4 min. Accepted.
- Emulating a module *behind* the tunnel adds ~2 ms each way to its responses; real modules answer
  ≪1 ms. Poll period is 15 ms and probe retry ~700 ms, so there should be ample margin — but this
  is the one timing risk worth verifying early.
