# Art-Net 4 integration reference

**Document type:** reusable protocol/integration reference  
**Status:** Protocol research and socket-free DMXWB protocol core are Confirmed; real UDP/runtime integration is Deferred to DEV-010  
**Last reviewed:** 2026-08-27  
**Specification basis:** Art-Net 4 Protocol Release V1.4, Document Revision 1.4dp, 23/10/2025

## 1. Purpose

This document records the Art-Net 4 behaviour relevant to implementing a reliable
Art-Net-to-DMX gateway and separates:

- behaviour from the reviewed Art-Net specification;
- DMXWB-specific product decisions;
- behaviour already Confirmed in the socket-free DEV-009 core;
- network/runtime behaviour Deferred to DEV-010 or production release.

The normative DMXWB requirements remain in `docs/TECHNICAL_SPEC.md`.

## 2. Labels

- **SPEC** — behaviour taken from the reviewed Art-Net 4 specification.
- **DMXWB** — project-specific design/product decision.
- **Confirmed DEV-009** — implemented in the deterministic socket-free core and covered by host tests.
- **Compatibility** — deliberate support for legacy/common behaviour beyond the modern preferred path.
- **Deferred DEV-010** — real UDP/runtime/recovery/source-switch integration.
- **Deferred production** — item required before production distribution.

## 3. Specification source

Primary source:

- Art-Net 4 Protocol Release V1.4, Document Revision 1.4dp, 23/10/2025
- https://art-net.org.uk/downloads/art-net.pdf

Official protocol/OEM site:

- https://art-net.org.uk/

When reusing this reference, verify the current Art-Net revision before coding.

## 4. Current DMXWB implementation boundary

**Confirmed DEV-009:**

```text
include/dmxwb/artnet_core.hpp
src/artnet_core.cpp
tests/test_artnet_core.cpp
commit d4ffe7ba6d9cbcc55fd012f41a9d1121f14284c0
```

DEV-009 accepts an already-received datagram plus source IPv4 and monotonic time.
It does not open sockets.

Confirmed core responsibilities:

```text
ArtDmx parse/state
ArtPoll parse/Targeted filtering
ArtPollReply packet construction
ArtSync state/staging
Sequence handling
source identity/conflict
3 s LOST/source-lock release
4 s sync fallback
512-channel network state
300-channel physical projection
```

**Deferred DEV-010:**

```text
UDP 6454 bind/rebind
actual receive/send
randomized unicast ArtPollReply scheduling
interface/network recovery
physical Source selector integration
real Art-Net controller/WB8 acceptance
```

## 5. Basic network model

**SPEC:**

```text
Art-Net 4
IPv4
UDP
port 6454 (0x1936)
protocol revision in current packet definitions: 14
```

**DMXWB:** one Art-Net output Port-Address is supported.

The constant `kArtNetUdpPort = 0x1936` exists in DEV-009 as protocol identity;
socket ownership begins in DEV-010.

## 6. Port-Address / universe numbering

Art-Net encodes a 15-bit Port-Address from Net + Sub-Net + Universe.

**SPEC:** current product range prefers `1..32767`; Port-Address `0` is deprecated
for sACN interoperability reasons.

**DMXWB compatibility decision:**

```text
configured range = 0..32767
default          = 0
```

Value `0` is a deliberate compatibility exception and should be labelled legacy
compatibility in UI/diagnostics.

**Confirmed DEV-009:** `ArtNetCore::create()` accepts `0..32767`, rejects values
above 15 bits, and exposes whether configured address is the legacy zero value.

## 7. Minimum packet set

DMXWB requires:

```text
ArtDmx
ArtPoll
ArtPollReply
ArtSync
```

**Confirmed DEV-009:** ArtDmx, ArtPoll and ArtSync are parsed by the core;
ArtPollReply is constructed by a deterministic builder.

## 8. Common validation and extensibility

For relevant packets validate:

```text
ID/signature = "Art-Net\0"
OpCode
protocol revision >= 14 where defined
minimum mandatory packet size
```

Receivers should not require arbitrary exact datagram size when the specification
only requires a minimum. Correct trailing extension bytes are ignored.

Reserved/unused receiver-do-not-test bits must not become invented rejection
conditions.

**Confirmed DEV-009:** common-header validation, old protocol rejection, safe
minimum-size checks, unsupported OpCode ignore and valid trailing ArtDmx bytes are
covered by host tests.

## 9. ArtDmx validation

ArtDmx header before Data is 18 bytes.

Required checks:

```text
Port-Address = configured target
Length       = even, 2..512
datagram     >= 18 + Length
```

Wrong Port-Address is ignored and does not acquire source lock.

**Confirmed DEV-009:** odd, zero, >512 and truncated Length cases are rejected.
A valid packet may include trailing bytes after the declared DMX Data.

## 10. Persistent 512-channel state

**SPEC:** ArtDmx Length is an even number from 2 to 512.

**DMXWB:**

```text
artnet_state[512]
```

For a valid short packet:

```text
channels 1..Length     -> updated
channels Length+1..512 -> retain previous values
```

**Confirmed DEV-009:** full 512-channel state is retained. A later two-byte packet
changes only channels 1 and 2; channels 3..512 Hold Last.

## 11. DMXWB physical projection

DMXWB product profile:

```text
network Art-Net state = 512 channels
physical RS-485 limit = first 300 slots
physical cadence      = fixed 44 Hz
```

Channels `301..512` are valid network state and are not considered malformed.

**Confirmed DEV-009:** `build_physical_snapshot()` returns a 300-slot
`DmxSnapshot` after a valid ArtDmx and copies network channels 1..300.

Before the first valid ArtDmx it returns no snapshot. This is intentional: future
`MQTT -> ART-NET` switching must preserve the existing physical frame rather than
invent a zero frame.

## 12. Modern unicast subscription

**SPEC:** modern Art-Net controllers discover subscribers through ArtPoll and
ArtPollReply and normally send ArtDmx unicast to subscribers.

Architectural consequence:

```text
Source = MQTT
ArtPollReply still advertises configured SwOut
ArtDmx still updates background artnet_state
physical output still uses MQTT
```

**Confirmed DEV-009:** PollReply construction keeps configured subscription
independent from the `artnet_output_active` status flag.

**Deferred DEV-010:** actual unicast reception/transmission and subscription
behaviour over the network.

### Compatibility receiver behaviour

DMXWB may accept correctly formed legacy broadcast ArtDmx as compatibility input.
Production interoperability must not depend on broadcast ArtDmx.

Broadcast-vs-unicast address inspection belongs to network runtime, not the
socket-free packet core.

## 13. ArtPoll and Targeted Mode

**SPEC:** ArtPoll is discovery/subscription traffic and may be broadcast or
unicast in Targeted Mode.

In Targeted Mode a node replies only if one of its subscribed Port-Addresses lies
inside the requested inclusive range.

**DMXWB:** one output makes this a simple range membership test.

**Confirmed DEV-009:** normal ArtPoll requests a reply; Targeted ArtPoll outside
the configured range is ignored.

## 14. ArtPollReply transmission

**SPEC:** reply is unicast and should be delayed randomly up to one second to
avoid response bunching.

**Confirmed DEV-009:** deterministic packet construction only.

**Deferred DEV-010:**

```text
non-blocking randomized delay 0..1 s
unicast send to ArtPoll sender
```

Never sleep in the physical DMX output thread for discovery timing.

## 15. ArtPollReply fields

DMXWB advertises one output subscription.

Key project requirements:

```text
SwOut             = configured output Port-Address
RefreshRate        = 44
subscription       = advertised regardless of application Source
GoodOutput active  = true only when Art-Net data is actually selected/output
```

**Confirmed DEV-009:** builder creates the fixed-size PollReply structure,
advertises one output, reports 44 Hz and accepts `artnet_output_active` separately
from subscription identity.

Exact network send semantics remain DEV-010.

## 16. OEM Code

A production Art-Net product requires its real registered OEM Code.

**DMXWB rule:**

```text
never invent production OEM code
```

**Confirmed DEV-009:** `ArtNetPollReplyIdentity` stores OEM as an explicit optional
value. `build_art_poll_reply()` refuses to build a reply when no OEM Code is
provided.

**Deferred production:**

- register DMXWB/product OEM Code;
- insert registered value in production identity;
- add the exact current required Art-Net credit to user documentation.

A development run may use an explicitly marked non-production value only when the
test procedure makes that status clear.

## 17. No ArtDmx FIFO

Wrong gateway architecture:

```text
ArtDmx #100 -> queue
ArtDmx #101 -> queue
ArtDmx #102 -> queue
physical DMX drains queue later
```

That can accumulate latency.

DMXWB architecture:

```text
ArtDmx -> latest committed Art-Net state
physical 44 Hz frame boundary -> acquire latest whole state
```

Intermediate states may be superseded.

**Confirmed DEV-009:** protocol core keeps current committed/staging state rather
than a network-frame FIFO.

**Deferred DEV-010:** connect this state to the existing whole-frame physical
mailbox/source selector.

## 18. Independent physical clock

ArtDmx arrival never directly starts UART transmission.

```text
network event:
ArtDmx -> validate -> Art-Net state

physical event:
44 Hz boundary -> acquire selected source snapshot -> serial DMX
```

This keeps UDP jitter away from DMX frame-start timing.

DEV-009 contains no UART interaction. DEV-010 must preserve the already Confirmed
independent `DmxOutput` 44 Hz scheduler.

## 19. Sequence handling

ArtDmx has an 8-bit Sequence.

**SPEC:**

```text
0x00       -> checking disabled
0x01..0xFF -> incrementing domain
```

**DMXWB:**

- stale/out-of-order data must not overwrite newer state;
- rollover `FF -> 01` must work;
- missing numbers are not waited for;
- no reorder FIFO;
- tracking resets when active-source lock is released.

**Confirmed DEV-009:** zero-disable, non-zero baseline, rollover, stale reject and
sequence gaps are covered by host tests.

## 20. Source identity

`Physical` distinguishes physical inputs that may share IP and Port-Address.

**DMXWB:**

```text
ArtNetSource = source IPv4 + ArtDmx.Physical
```

**Confirmed DEV-009:** both fields are part of equality/source-lock logic.

## 21. Multiple sources

DMXWB chooses a documented conflict policy instead of automatic merge:

```text
no active source
    -> first valid source becomes ACTIVE

same source
    -> packet accepted

different IPv4 or Physical
    -> CONFLICT
    -> second source does not mutate committed output
```

No HTP/LTP merge is implemented.

**Confirmed DEV-009:** IP conflict and same-IP/different-Physical conflict are
covered by host tests.

## 22. ArtSync

Startup:

```text
asynchronous
ArtDmx -> committed state
```

After valid ArtSync from the relevant source IP:

```text
synchronous
ArtDmx -> staging only
next ArtSync -> staging atomically becomes committed
```

ArtSync does not directly start UART and does not move the 44 Hz physical grid.

If ArtSync is absent for 4 seconds or more:

```text
return to asynchronous mode
```

Mismatched ArtSync source IP is ignored.

**Confirmed DEV-009:** source matching, enter-sync, staging, atomic commit and
4-second fallback are covered by host tests.

## 23. Hold Last and source loss

DMXWB source-lock timeout:

```text
3 s
```

On LOST:

```text
do not clear committed artnet_state
do not blackout
do not automatically change application Source
Hold Last
release stale source lock
reset sequence/sync tracking
```

**Confirmed DEV-009:** `tick(now)` performs deterministic LOST transition and lock
release while preserving committed channels.

**Deferred DEV-010:** real network-loss scenarios and recovery on WB8.

## 24. Application Source selector

DMXWB has:

```text
WB MQTT
ART-NET
```

Inactive source continues to update.

When Source=MQTT, Art-Net network/runtime must stay alive and keep background
state/subscription current. When Source=ART-NET, MQTT logical model continues to
update.

Switch occurs only at a physical frame boundary.

**Confirmed before DEV-009:** MQTT side already preserves background logical state
while `Source=artnet` and does not create fake MQTT physical frames.

**Confirmed DEV-009:** Art-Net core can provide latest whole 300-slot projection
and intentionally provides no frame before first valid ArtDmx.

**Deferred DEV-010:** source arbitration wiring between Art-Net core and
`DmxOutput`.

## 25. GoodOutput and discovery state

Subscription and actual output state are different concepts.

DMXWB contract:

```text
SwOut remains advertised while Source=MQTT
GoodOutput/data-active reflects actual selected Art-Net physical output
```

**Confirmed DEV-009:** builder API keeps these inputs separate.

DEV-010 must supply the real runtime state when building replies.

## 26. Network recovery

Required future recovery cases:

- Ethernet cable disconnect/reconnect;
- interface down/up;
- source controller restart/power cycle;
- source IP change;
- temporary UDP socket failure;
- local rebind need.

The network worker may recreate/rebind UDP 6454 while physical DMX continues Hold
Last.

**Deferred DEV-010:** none of these network cases are claimed by DEV-009.

## 27. Implementation checklist

### Confirmed DEV-009

1. Validate `Art-Net\0`.
2. Parse supported OpCodes.
3. Enforce protocol revision >=14 where applicable.
4. Decode one 15-bit Port-Address.
5. Preserve explicit Port-Address 0 compatibility.
6. Validate ArtDmx Length even `2..512`.
7. Validate minimum datagram length.
8. Ignore valid trailing extension bytes.
9. Maintain persistent 512-channel state.
10. Project channels 1..300 to physical snapshot.
11. Keep no physical Art-Net snapshot before first valid ArtDmx.
12. Implement Sequence=0 semantics.
13. Implement non-zero rollover/stale protection without waiting for gaps.
14. Include `Physical` in source identity.
15. Implement `WAITING/ACTIVE/LOST/CONFLICT`.
16. Apply documented no-merge conflict policy.
17. Implement ArtSync staging/commit.
18. Implement 4-second sync fallback.
19. Ignore mismatched ArtSync source.
20. Implement Hold Last and 3-second lock release.
21. Parse ArtPoll and Targeted Mode.
22. Build one-output ArtPollReply.
23. Advertise RefreshRate 44.
24. Require explicit OEM identity.

### Deferred DEV-010 / production

25. Bind IPv4 UDP 6454.
26. Receive real ArtDmx/ArtPoll/ArtSync.
27. Schedule randomized non-blocking unicast PollReply.
28. Recover socket/interface without process restart.
29. Integrate physical Source switching.
30. Verify no ArtDmx FIFO latency under real traffic.
31. Verify cable/interface/source restart/IP-change recovery.
32. Test with named/versioned external Art-Net controller(s).
33. Test second real conflicting source.
34. Verify discovery/subscription while physical Source=MQTT.
35. Verify real physical Hold Last while Source=ART-NET.
36. Register product OEM Code before release.
37. Add required Art-Net credit to production user documentation.

## 28. Relevant DMXWB documents

Normative project contract:

```text
docs/TECHNICAL_SPEC.md       §16 Art-Net
docs/PROJECT_STATE.md        current confirmed implementation point
docs/ROADMAP.md              DEV-009 / DEV-010
```

Current implementation base:

```text
d4ffe7ba6d9cbcc55fd012f41a9d1121f14284c0
Implement DEV-009 Art-Net protocol core
```

The protocol parser/state machine is now **Confirmed** by DEV-009 host tests.
Real UDP transport, runtime recovery, source-selector wiring and WB8 Art-Net
interoperability remain **Deferred DEV-010** and must not be described as already
Confirmed.

## 29. External references

Official Art-Net specification:

- https://art-net.org.uk/downloads/art-net.pdf

Official Art-Net site / OEM registration / product credit requirements:

- https://art-net.org.uk/

When implementing or reusing this design, verify the current specification
revision before coding.
