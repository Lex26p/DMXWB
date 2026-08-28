# Art-Net 4 integration reference

**Document type:** reusable protocol/integration reference  
**Status:** Art-Net protocol core and DEV-010 Linux UDP/runtime/physical integration are Confirmed; production OEM registration remains Deferred  
**Last reviewed:** 2026-08-28  
**Specification basis:** Art-Net 4 Protocol Release V1.4, Document Revision 1.4dp, 23/10/2025

## 1. Purpose

This document records the Art-Net 4 behaviour relevant to a reliable
Art-Net-to-DMX gateway and separates:

- behaviour taken from the reviewed Art-Net specification;
- portable implementation principles;
- DMXWB-specific product decisions;
- behaviour Confirmed in DEV-009/DEV-010;
- production-only items that remain Deferred.

The normative DMXWB requirements remain in `docs/TECHNICAL_SPEC.md`.

## 2. Labels

- **SPEC** — behaviour taken from the reviewed Art-Net specification.
- **Portable** — engineering guidance reusable in another gateway.
- **DMXWB** — project-specific product decision.
- **Confirmed DEV-009** — deterministic socket-free protocol core.
- **Confirmed DEV-010** — Linux UDP/runtime/source/physical integration proven by
  real WB8 acceptance.
- **Deferred production** — required before production distribution but not part
  of the DEV-010 engineering gate.

## 3. Specification source and attribution

Primary specification:

```text
Art-Net 4 Protocol Release V1.4
Document Revision 1.4dp
23/10/2025
https://art-net.org.uk/downloads/art-net.pdf
```

Official protocol/OEM site:

```text
https://art-net.org.uk/
```

The official Art-Net site states that a user guide for a product implementing
Art-Net must include the credit:

> Art-Net™ Designed by and Copyright Artistic Licence Engineering Ltd

When reusing this reference, verify the current Art-Net revision, attribution
wording and OEM requirements before release.

## 4. Current DMXWB implementation boundary

### Confirmed DEV-009

Socket-free protocol core:

```text
include/dmxwb/artnet_core.hpp
src/artnet_core.cpp
tests/test_artnet_core.cpp
```

Core responsibilities:

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

### Confirmed DEV-010

Linux/runtime/source path:

```text
LinuxArtNetDatagramTransport
-> ArtNetRuntime
-> ArtNetCore
-> latest immutable DmxSnapshot
-> ArtNetSourceCoordinator
-> DmxSourceRouter
-> DmxOutput
```

Confirmed by real WB8/network/physical acceptance:

- UDP 6454 bind/receive/send;
- randomized non-blocking unicast PollReply scheduling;
- real QLC+ 5.2.2 discovery/subscription;
- physical MQTT/ART-NET source switching;
- Hold Last and same-process recovery;
- WB interface down/up recovery;
- QLC+ restart recovery;
- source IPv4 change across two real networks;
- IP+Physical conflict/no-merge;
- real UDP ArtSync staging/release;
- repeated reconnect;
- latest/no-FIFO high-rate stress;
- physical fixed 44 Hz throughout acceptance.

## 5. Basic network model

**SPEC:**

```text
Art-Net 4
IPv4
UDP
port 6454 (0x1936)
protocol revision >= 14 for the packet set used here
```

**DMXWB:** one Art-Net output Port-Address is supported.

**Confirmed DEV-010:** the Linux runtime binds UDP 6454 and processes network
datagrams without placing socket/network work in the physical DMX thread.

## 6. Port-Address / universe numbering

Art-Net encodes a 15-bit Port-Address from Net + Sub-Net + Universe.

**SPEC:** current product range is `1..32767`; Port-Address `0` is deprecated for
sACN interoperability reasons.

**DMXWB compatibility decision:**

```text
configured range = 0..32767
default          = 0
```

Value `0` is an explicit compatibility exception, not a change to Art-Net.

## 7. Minimum packet set

DMXWB supports:

```text
ArtDmx
ArtPoll
ArtPollReply
ArtSync
```

ArtDmx/ArtPoll/ArtSync are parsed; ArtPollReply is built and transmitted by the
runtime.

## 8. Validation and extensibility

For relevant packets validate:

```text
ID/signature = "Art-Net\0"
OpCode
protocol revision >= 14 where defined
minimum mandatory packet size
```

Portable rule: do not invent exact packet-length checks when the protocol defines
a minimum and allows trailing extension bytes.

Confirmed:

- invalid ID rejected;
- old protocol revision rejected where applicable;
- unsupported opcode safely ignored;
- valid trailing ArtDmx bytes accepted;
- reserved/do-not-test fields do not become invented reject conditions.

## 9. ArtDmx validation and persistent state

ArtDmx header before Data is 18 bytes.

Required checks:

```text
Port-Address = configured target
Length       = even, 2..512
datagram     >= 18 + Length
```

DMXWB keeps:

```text
artnet_state[512]
```

For a valid short packet:

```text
channels 1..Length     -> updated
channels Length+1..512 -> Hold Last
```

Wrong Port-Address does not acquire the source lock.

## 10. Physical projection

DMXWB product profile:

```text
network Art-Net state = 512 channels
physical RS-485 limit = first 300 slots
physical cadence      = fixed 44 Hz
```

Channels `301..512` remain valid network state but are not transmitted by this
physical product profile.

Before the first valid ArtDmx, no Art-Net physical snapshot exists. Therefore an
explicit MQTT -> ART-NET selection before the first ArtDmx preserves the current
physical frame instead of inventing a zero frame.

## 11. Modern unicast subscription

Portable modern Art-Net flow:

```text
controller ArtPoll
-> node ArtPollReply
-> controller sends ArtDmx to discovered subscriber
```

DMXWB keeps subscription/discovery alive while application `Source=MQTT`.

Confirmed DEV-010:

- QLC+ discovers the DMXWB node;
- PollReply advertises the configured output;
- ArtDmx continues to update background Art-Net state while MQTT is selected.

## 12. ArtPoll and Targeted Mode

Normal ArtPoll requests a reply.

In Targeted Mode, reply is produced only if the node's subscribed Port-Address is
inside the requested inclusive range.

DMXWB has one output, so this reduces to one range-membership test.

Confirmed in core tests and real runtime discovery.

## 13. PollReply scheduling

Portable rule: PollReply response timing must not block the physical output
thread.

**SPEC/current requirement used by DMXWB:**

```text
unicast reply
random delay <= 1 s
```

**Confirmed DEV-010:**

```text
ArtPoll
-> schedule due time
-> continue network/runtime work
-> later unicast ArtPollReply
```

No sleep is introduced into `DmxOutput`.

## 14. PollReply fields / output state

DMXWB advertises one output subscription.

Project contract:

```text
SwOut             = configured Port-Address
RefreshRate        = 44
subscription       = advertised regardless of application Source
GoodOutput active  = true only when Art-Net is actually selected/output
```

Subscription existence and physical-output activity are intentionally separate
state.

## 15. OEM Code

A production Art-Net product requires a real registered OEM Code.

DMXWB rule:

```text
never invent production OEM code
```

The PollReply identity requires an explicit OEM value.

DEV-010 engineering acceptance used an explicitly labelled development-only
placeholder. This is not a production assignment.

**Deferred production:**

- register the actual product OEM Code;
- insert that registered value in production identity;
- re-check current Art-Net OEM/credit requirements before distribution.

## 16. Latest-state architecture / no ArtDmx FIFO

Wrong gateway architecture:

```text
ArtDmx #100 -> queue
ArtDmx #101 -> queue
ArtDmx #102 -> queue
physical output drains old packets later
```

This accumulates latency.

DMXWB architecture:

```text
ArtDmx
-> committed Art-Net state / latest immutable snapshot
-> coordinator samples latest generation
-> source router publishes current whole selected snapshot
-> independent 44 Hz DmxOutput frame boundary
```

Intermediate network states may be superseded.

### Confirmed DEV-010 stress evidence

Accepted C6 run:

```text
datagrams_received:             4404
committed snapshots published:  4396
router Art-Net snapshots:       449
burst packets:                  4096
burst duration:                 473 ms
```

A final BLUE guard became stable without multi-second playback of old burst
states. This is direct real-runtime evidence for latest/coalescing behaviour.

## 17. Independent physical clock

Network event:

```text
ArtDmx
-> validate/update Art-Net state
```

Physical event:

```text
fixed 44 Hz frame boundary
-> acquire latest whole selected source snapshot
-> serial DMX
```

ArtDmx arrival does not directly start UART transmission.

DEV-010 network, source-loss, ArtSync and burst tests all retained:

```text
active_refresh_hz = 44
missed_deadlines  = 0
```

in their accepted physical runs.

## 18. Sequence handling

ArtDmx Sequence:

```text
0x00       -> ordering check disabled
0x01..0xFF -> incrementing domain
```

DMXWB:

- rejects stale/out-of-order state;
- supports rollover `FF -> 01`;
- does not wait for missing sequence numbers;
- does not create a reorder FIFO;
- resets tracking when the active source lock is released.

Confirmed by DEV-009 deterministic tests; runtime preserves the same core logic.

## 19. Source identity

DMXWB source identity:

```text
ArtNetSource = source IPv4 + ArtDmx.Physical
```

Both fields participate in lock equality.

## 20. Multiple sources / conflict

Policy:

```text
no active source
    -> first valid source becomes ACTIVE

same source
    -> accepted

different IPv4 or Physical
    -> CONFLICT
    -> no merge
    -> conflicting packet does not mutate active committed state
```

No HTP/LTP merge is performed.

Confirmed DEV-010C4:

```text
conflicting packets: 150
physical takeover:   none
merge:               none
critical errors:     0
```

## 21. ArtSync

Startup mode:

```text
asynchronous
ArtDmx -> committed
```

After a relevant valid ArtSync:

```text
synchronous
ArtDmx -> staging
next ArtSync -> atomic staging commit
```

If ArtSync is absent for 4 seconds, DMXWB returns to asynchronous mode and
deterministically commits staged state as specified by the core design.

Mismatched ArtSync source IP is ignored.

### Confirmed real UDP acceptance

DEV-010C5:

```text
first ArtSync
-> RED staged for 2530 ms / 51 packets
-> physical remains GREEN
-> second ArtSync
-> physical transitions once to RED
```

The physical 44 Hz schedule continued independently.

## 22. Hold Last and source loss

DMXWB source-lock timeout:

```text
3 s
```

On LOST:

```text
keep committed artnet_state
no blackout
do not change application Source
Hold Last
release stale source lock
reset sequence/sync tracking
```

Confirmed on real WB8 through:

- QLC+ output loss;
- full QLC+ restart;
- WB interface down/up;
- repeated reconnect;
- primary-source release before source IPv4 change.

## 23. Application Source selector

Application Source values:

```text
mqtt
artnet
```

Inactive source continues updating.

### ART-NET selected

- MQTT logical Fixture/Group/Scene state still mutates;
- MQTT whole logical snapshot remains current;
- physical output uses Art-Net.

### MQTT selected

- Art-Net UDP receiver/discovery remain alive;
- Art-Net network state continues updating;
- physical output uses MQTT.

Switches are whole-snapshot publications and are observed only by the physical
output at frame boundaries.

Confirmed by real DEV-010B physical acceptance.

## 24. GoodOutput and subscription state

These are separate concepts:

```text
subscription advertised != Art-Net currently selected/output
```

DMXWB publishes GoodOutput/data-active only after an Art-Net snapshot has
successfully reached the selected physical source path.

Returning to MQTT clears the Art-Net physical-active indication while keeping the
subscription advertised.

## 25. Network recovery

Confirmed DEV-010 real cases:

- controller traffic loss >3 s;
- controller full restart;
- WB interface down/up;
- repeated source reconnect;
- source IPv4 change;
- conflict source arrival.

The application process does not need an operator restart for these temporary
network/source failures.

The transport abstraction also supports close/rebind recovery after socket
receive/send failure.

## 26. Real source IPv4 change

DEV-010C3 used two real network paths simultaneously:

```text
primary:
10.200.200.2 -> 10.200.200.1

Wi-Fi:
192.168.42.160 -> 192.168.42.1
```

The old primary source was disabled and allowed to exceed the 3 s LOST timeout
before the Wi-Fi source was enabled.

Confirmed:

- distinct Windows interfaces;
- distinct WB `dbg0` / `wlan0` routes;
- old source lock released;
- application Source stayed ART-NET;
- same DMXWB PID/start time;
- physical GREEN Hold Last during loss;
- new source IP accepted as RED;
- conflicts remained zero;
- physical output stayed 44 Hz.

## 27. DEV-010 acceptance reports

```text
docs/DEV010A_ARTNET_QLCPLUS_NETWORK_REPORT.txt
docs/DEV010B_ARTNET_MQTT_SOURCE_SWITCH_REPORT.txt
docs/DEV010B_ARTNET_LOST_HOLD_RECOVERY_REPORT.txt
docs/DEV010C1_WB8_INTERFACE_RECOVERY_REPORT.txt
docs/DEV010C2_QLCPLUS_RESTART_RECOVERY_REPORT.txt
docs/DEV010C3_ARTNET_SOURCE_IP_CHANGE_REPORT.txt
docs/DEV010C4_ARTNET_CONFLICT_REPORT.txt
docs/DEV010C5_ARTSYNC_REPORT.txt
docs/DEV010C6_RECONNECT_LATEST_REPORT.txt
```

External controller:

```text
QLC+ 5.2.2
```

Acceptance WB8:

```text
Wiren Board rev. 8.5.1 (T507)
Debian 11 Bullseye
wb-2606 stable
kernel 6.8.0-wb160
aarch64
/dev/ttyRS485-1 -> ttyS2
```

## 28. Portable implementation checklist

1. Validate `Art-Net\0`, opcode and protocol revision.
2. Validate mandatory minimum packet sizes.
3. Accept valid extension/trailing bytes.
4. Keep persistent network-channel state.
5. Keep source identity separate from application Source selection.
6. Define deterministic multi-source conflict policy.
7. Do not merge unless merge semantics are explicitly part of the product.
8. Release stale source lock without clearing committed output.
9. Keep a deliberate Hold Last policy.
10. Keep ArtSync staging separate from physical UART timing.
11. Keep network receive cadence independent from physical DMX cadence.
12. Route latest whole state instead of replaying old network frames.
13. Keep discovery/subscription alive when the network source is not physically
    selected.
14. Keep PollReply timing off the physical output thread.
15. Design socket transport for close/rebind recovery.
16. Test interface loss, controller restart, source identity change and conflict.
17. Test burst traffic for accumulating latency.
18. Verify physical output timing while all network failure tests run.
19. Use a real registered OEM Code before production.
20. Include the current required Art-Net user-guide credit.

## 29. Current DMXWB status

```text
DEV-009 protocol core:   Confirmed
DEV-010 Linux runtime:   Confirmed
DEV-010 physical source: Confirmed
DEV-010 network recovery: Confirmed
DEV-010 ArtSync:         Confirmed
DEV-010 no-FIFO stress:  Confirmed
production OEM Code:     Deferred
```

Current engineering gate after DEV-010 closeout:

```text
DEV-011 — static MQTT-only Web UI
```
