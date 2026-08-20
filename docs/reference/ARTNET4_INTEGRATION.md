# Art-Net 4 integration reference

**Document type:** reusable protocol/integration reference  
**Status:** Protocol research and DMXWB design contract complete; DMXWB Art-Net runtime is not implemented yet  
**Last reviewed:** 2026-08-20  
**Specification basis:** Art-Net 4 Protocol Release V1.4, Document Revision 1.4dp, 23/10/2025

## 1. Purpose

This document records the Art-Net 4 behaviour relevant to implementing a reliable Art-Net-to-DMX gateway and clearly separates:

- requirements or behaviour from the current Art-Net specification;
- deliberate design choices made by DMXWB;
- details that remain deferred until the Art-Net implementation gates.

It is intended to be reusable in later projects. DMXWB-specific physical limits are shown as examples, not presented as Art-Net limitations.

The normative project requirements remain in `docs/TECHNICAL_SPEC.md`.

## 2. Labels used in this document

- **SPEC** — behaviour taken from the reviewed Art-Net 4 specification.
- **DMXWB** — project-specific implementation/design decision.
- **Compatibility** — deliberately supports legacy/common behaviour beyond the modern preferred path.
- **Deferred** — must be completed before production release but is not implemented yet.

## 3. Specification source

Primary source:

- Art-Net 4 Protocol Release V1.4, Document Revision 1.4dp, 23/10/2025
- https://art-net.org.uk/downloads/art-net.pdf

Official protocol/OEM site:

- https://art-net.org.uk/

When this document is reused later, re-check the current Art-Net revision. Protocol details can change while this reference remains historically correct for revision `1.4dp`.

## 4. Basic network model

**SPEC:**

```text
Art-Net 4
IPv4
UDP
port 6454 (0x1936)
protocol revision in current packet definitions: 14
```

Art-Net transports DMX512/RDM-oriented data over Ethernet.

**DMXWB:** one Art-Net output Port-Address is supported.

## 5. Port-Address / universe numbering

Art-Net encodes the Port-Address as a 15-bit value:

```text
Net + Sub-Net + Universe
```

**SPEC:** current valid product range is `1..32767`; Port-Address `0` is deprecated to improve sACN compatibility.

**DMXWB compatibility decision:**

```text
configured range = 0..32767
default          = 0
```

Value `0` is retained only as an explicit compatibility mode for controllers that commonly use zero-based numbering. UI/diagnostics should label it as legacy compatibility rather than implying that zero is the preferred current Art-Net value.

## 6. Minimum required DMXWB packet set

The planned DMXWB receiver/node supports:

```text
ArtDmx
ArtPoll
ArtPollReply
ArtSync
```

**DMXWB:** `ArtPoll`/`ArtPollReply` are not decorative discovery only. They participate in the current Art-Net unicast subscription model.

## 7. ArtDmx packet validation

### 7.1 Core checks

For an ArtDmx packet, validate at least:

```text
ID/signature = "Art-Net\0"
OpCode       = ArtDmx / OpOutput
protocol revision >= 14
Port-Address = configured target
Length       = even number, 2..512
UDP datagram contains at least header + declared data
```

ArtDmx header size before Data is 18 bytes, therefore:

```text
minimum datagram size = 18 + Length
```

### 7.2 Extensibility and trailing bytes

**SPEC:** Art-Net packet definitions are designed to remain extensible. Receivers should validate minimum packet length; bytes after the end of an otherwise valid packet are ignored. Reserved/unused fields described as transmit-zero / receiver-do-not-test must not be turned into arbitrary reject conditions.

Therefore do not implement:

```text
udp_size == exact_expected_size
```

when the specification only requires a minimum.

Prefer:

```text
udp_size >= required_minimum
```

with all declared payload bytes present.

## 8. ArtDmx Length and persistent channel state

**SPEC:** ArtDmx `Length` is an even number from `2` to `512`.

A gateway may choose its own physical DMX output length; ArtDmx itself still carries standard DMX channel data.

**DMXWB network state:**

```text
artnet_state[512]
```

If a valid packet has `Length < 512`:

```text
channels 1..Length -> updated
remaining channels -> retain previous values
```

This produces persistent per-channel state rather than clearing all channels not present in a short packet.

**DMXWB physical example:**

```text
network state capacity = 512
physical WB8 output    = channels 1..300 only
```

Channels `301..512` remain valid Art-Net state and are not transmitted by the current DMXWB physical profile.

This `300` limit is a DMXWB hardware/product decision, not an Art-Net rule.

## 9. Modern unicast subscription

**SPEC:** ArtDmx is sent unicast to devices subscribed to the relevant universe. The transmitting controller regularly uses ArtPoll to discover subscription changes. A subscriber lists its universe in ArtPollReply `SwIn`/`SwOut`. If there are no subscribers, the controller should not send ArtDmx. The current spec does not allow broadcast ArtDmx transmission.

This has an important architectural consequence:

> ArtPollReply must advertise the configured output universe even if the application is temporarily using another local source for its physical output.

Otherwise a conforming Art-Net controller can legitimately stop sending ArtDmx because it no longer sees a subscriber.

**DMXWB:**

```text
Source = MQTT
ArtPollReply still advertises configured SwOut
ArtDmx continues updating background artnet_state
physical output still uses MQTT
```

### Compatibility receiver behaviour

**DMXWB compatibility:** a correctly formed legacy broadcast ArtDmx may be accepted as input so older controllers remain usable, but production interoperability must not depend on broadcast and DMXWB does not treat broadcast as the modern conformance path.

## 10. ArtPoll and ArtPollReply

### 10.1 ArtPoll

**SPEC:** ArtPoll is used for discovery and subscription management. It may use directed broadcast for broad discovery and unicast in Targeted Mode.

Consumers of ArtPoll must tolerate the defined minimum packet length and historical extensions.

### 10.2 Targeted Mode

If ArtPoll Targeted Mode is enabled, a node replies only when one of its subscribed Port-Addresses lies inside the requested inclusive range.

**DMXWB:** with one output Port-Address, this is a simple range membership test.

### 10.3 ArtPollReply transmission

**SPEC:**

- response is unicast;
- node should apply a random delay up to 1 second before replying to reduce response bunching in large networks.

Do not perform a blocking 1-second sleep in a DMX output thread. Discovery timing belongs to the network subsystem/timer mechanism.

## 11. RefreshRate and physical cadence

**SPEC:** `ArtPollReply.RefreshRate` describes the maximum ArtDmx rate the gateway can process. For a gateway outputting DMX512, the specification states the maximum DMX512 rate as `44 Hz`.

**DMXWB:**

```text
ArtPollReply.RefreshRate = 44
physical DMX cadence     = fixed 44 Hz
physical slots           = max 300
```

The equality of the network-advertised maximum and physical cadence is convenient, but it still does not mean that ArtDmx packet arrival directly starts UART transmission.

## 12. Do not use an ArtDmx FIFO

This is one of the most important gateway design decisions.

Wrong architecture:

```text
ArtDmx #100 -> queue
ArtDmx #101 -> queue
ArtDmx #102 -> queue
physical DMX drains queue later
```

If network input is temporarily faster than physical output, latency grows without bound.

Preferred DMXWB architecture:

```text
ArtDmx #100 -> latest state
ArtDmx #101 -> replaces it
ArtDmx #102 -> replaces it

physical frame boundary -> acquire latest whole committed snapshot
```

Intermediate network states may be superseded.

This creates bounded sampling latency instead of accumulated queue latency.

## 13. Independent physical output clock

**DMXWB:** ArtDmx arrival never directly starts physical serial transmission.

```text
network event:
ArtDmx -> validate -> update Art-Net state/snapshot

physical event:
44 Hz frame boundary -> acquire latest committed physical snapshot -> send DMX
```

Benefits:

- network jitter does not become DMX frame-start jitter;
- UDP bursts do not create bursts of serial transmission;
- Hold Last is natural;
- source switching can occur atomically on physical frame boundaries.

Even if both nominal rates are `44 Hz`, their phases are independent.

## 14. Sequence handling

ArtDmx contains an 8-bit `Sequence`.

**SPEC:**

```text
Sequence = 0x00 -> sequence checking disabled
Sequence = 0x01..0xFF -> incrementing sequence domain
```

The purpose is to protect against out-of-order arrival.

**DMXWB decision:**

- a newer valid packet may update state;
- a stale/out-of-order packet must not overwrite newer state;
- rollover `0xFF -> 0x01` is handled;
- missing sequence numbers are not waited for;
- there is no reorder FIFO that delays output;
- sequence tracking resets when the active-source lock is released.

The goal is stale-packet protection, not guaranteed delivery. Art-Net is carried over UDP.

## 15. `Physical` is part of source identity

The ArtDmx `Physical` field identifies the physical DMX input port that generated the network data.

**SPEC:** it lets a receiver distinguish streams with identical Port-Address that may originate from different physical inputs, including multiple inputs behind one IP address.

Therefore source identity should not be only:

```text
source IPv4
```

**DMXWB:**

```text
ArtNetSourceIdentity = source IPv4 + Physical
```

This prevents two different physical inputs on one gateway IP from being mistaken for one stream.

## 16. Multiple ArtDmx sources

**SPEC:** when the same Port-Address arrives from different IP addresses, or from different `Physical` inputs on the same IP, a potential conflict exists. A node may legitimately:

1. report/treat it as an error condition and wait for user intervention; or
2. automatically merge the streams.

The specification prefers merge for richer functionality, but permits the error policy if documented.

**DMXWB decision:**

```text
no active source
    -> first valid source becomes ACTIVE

same source
    -> packets accepted

different IP or different Physical
    -> CONFLICT
    -> second source does not modify output state
```

DMXWB does not implement HTP/LTP merge.

This policy must be visible in product documentation.

## 17. ArtSync

ArtSync solves a different problem from frame-rate matching: it lets multiple ArtDmx updates become visible together.

### 17.1 Specification behaviour

**SPEC:**

- controller sends ArtSync by directed broadcast;
- node uses received ArtSync to transfer previously buffered ArtDmx data to output;
- once synchronous behaviour is active, ArtDmx is buffered until the next ArtSync;
- if ArtSync is absent for 4 seconds or more, node returns to non-synchronous operation;
- source IP of ArtSync must match the source IP of the relevant/recent ArtDmx stream; mismatched ArtSync is ignored.

### 17.2 DMXWB mapping

Startup is asynchronous:

```text
valid ArtDmx
-> update committed Art-Net state
```

After a valid ArtSync engages synchronous mode:

```text
ArtDmx
-> update staging state only

next valid ArtSync
-> staging atomically becomes committed

next physical 44 Hz boundary
-> output latest committed snapshot
```

ArtSync itself does **not** directly start the UART and does not move the physical 44 Hz frame grid.

This preserves both:

- network-level synchronized commit semantics;
- deterministic independent physical DMX cadence.

## 18. Hold Last and loss detection

**SPEC:** in the absence of new ArtDmx, a DMX output port continuously retransmits the same frame. An active but unchanged input is expected to re-transmit its last ArtDmx periodically; the reviewed spec recommends roughly `800..1000 ms` for convergence with sACN behaviour.

**DMXWB decision:**

```text
source-lock / LOST timeout = 3 s
```

On LOST:

```text
do not clear artnet_state
do not generate blackout
do not automatically switch application Source
hold last committed physical state
release stale Art-Net source lock
reset sequence/sync tracking for that source
```

The next valid allowed source can become ACTIVE after the lock is released.

## 19. Source selector and background reception

DMXWB has an application-level source selector:

```text
WB MQTT
ART-NET
```

The inactive source continues operating.

When physical Source is MQTT:

```text
Art-Net socket remains active
ArtPollReply subscription remains active
ArtDmx continues updating artnet_state
Art-Net link/source diagnostics continue
physical DMX reads MQTT snapshot
```

When physical Source is Art-Net:

```text
MQTT logical state continues to update
physical DMX reads latest committed Art-Net snapshot
```

Switching occurs only on a physical frame boundary so one physical DMX frame never mixes data from two application sources.

This source selector is a DMXWB application concept, not an Art-Net protocol feature.

## 20. GoodOutput and discovery state

ArtPollReply should represent real gateway state, not merely configuration.

**DMXWB contract:** the output subscription (`SwOut`) remains advertised regardless of application source, while the output-status bits reflect whether Art-Net data is actually selected/being converted to physical DMX.

When implementing exact `GoodOutput` fields, use the current Art-Net revision directly rather than copying old bit definitions from third-party examples.

## 21. Network recovery

The Art-Net subsystem should not require application restart after common network events.

**DMXWB required recovery cases:**

- Ethernet cable disconnect/reconnect;
- network interface down/up;
- source controller restart or power cycle;
- source IP change;
- temporary UDP socket failure;
- local socket re-bind need.

The network worker may recreate and re-bind UDP port `6454` while the independent physical DMX loop continues Hold Last.

## 22. OEM Code and required credit

This is easy to overlook during development.

**SPEC / official Art-Net site:**

- a product implementing Art-Net requires a registered OEM Code;
- the OEM Code uniquely identifies a product and is used in ArtPoll/ArtPollReply;
- the product user guide must contain the required Art-Net credit.

**DMXWB Deferred:**

- obtain a real registered OEM Code before production distribution;
- use only an explicitly marked development placeholder before registration;
- do not ship a production bundle with an invented placeholder;
- include the exact current required credit text from the official Art-Net site in the final user documentation.

Official OEM/credit information:

- https://art-net.org.uk/

## 23. DMXWB physical-profile example

The current DMXWB WB8 output deliberately uses:

```text
network Art-Net state: 512 channels
physical output limit: 300 slots
physical cadence:      44 Hz
```

Why this is useful:

- Art-Net parsing remains standards-compatible up to 512 channels;
- physical WB8 timing is based on a separately hardware-proven profile;
- incoming channels `301..512` are not treated as malformed;
- the product can clearly document its physical connector/output limit.

A different product can keep the same Art-Net architecture and choose a different physical slot limit after measuring its own hardware.

## 24. Implementation checklist

Before calling an Art-Net receiver/gateway implementation complete:

1. Bind UDP 6454 on IPv4.
2. Validate `Art-Net\0`, OpCode and protocol revision.
3. Decode Port-Address correctly.
4. Validate ArtDmx Length as even `2..512`.
5. Validate minimum packet length, not arbitrary exact size.
6. Preserve trailing-extension compatibility.
7. Maintain persistent 512-channel Art-Net state.
8. Implement Sequence=0 and rollover-aware non-zero sequence handling.
9. Include `Physical` in source discrimination.
10. Implement the documented multiple-source policy.
11. Implement ArtPoll.
12. Implement unicast ArtPollReply.
13. Implement Targeted Mode.
14. Apply non-blocking randomized ArtPollReply delay.
15. Advertise the output subscription consistently.
16. Set the correct advertised refresh rate.
17. Implement ArtSync staging/commit and 4-second fallback.
18. Reject/ignore ArtSync from the wrong source IP.
19. Implement Hold Last.
20. Implement source-loss diagnostics/lock release.
21. Avoid ArtDmx FIFO latency.
22. Keep physical DMX cadence independent of UDP arrival.
23. Recover UDP socket/network operation without restarting the whole daemon.
24. Test with at least two independent Art-Net controller implementations.
25. Test sequence rollover, packet loss and reordering.
26. Test a second conflicting source.
27. Test short and full ArtDmx Length.
28. Test source switching while network reception continues.
29. Register the product OEM Code before release.
30. Add the required Art-Net credit to product documentation.

## 25. Relevant DMXWB documents

Current project contract:

```text
docs/TECHNICAL_SPEC.md       §16 Art-Net
docs/PROJECT_STATE.md        Art-Net decisions
docs/ROADMAP.md              DEV-009 / DEV-010
```

At the date of this reference document, the Art-Net parser/runtime has not yet been implemented. Therefore protocol research and architecture decisions are **Decided**, not a hardware/runtime **Confirmed** Art-Net implementation.

## 26. External references

Official Art-Net specification:

- https://art-net.org.uk/downloads/art-net.pdf

Official Art-Net site / OEM registration / product credit requirements:

- https://art-net.org.uk/

When implementing or reusing this design, verify the current specification revision before coding.
