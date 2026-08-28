# PROJECT_STATE

**Last updated:** 2026-08-28

## Repository base

```text
fe64b3627fdad9d8905ecbb9a5540cd80a364eff
Validate DEV-010C3 real-network source IP change
```

## Last confirmed engineering PASS

```text
DEV-010 — Art-Net runtime, reliability and Source switching
```

DEV-010 is Confirmed by host/integration work plus real WB8 physical/network
acceptance. The acceptance runtime, QLC+ 5.2.2 and deterministic probes proved the
roadmap requirements for discovery/subscription, physical source switching,
ArtSync, network loss/recovery, source restart/IP change, WB interface down/up,
repeated reconnect, conflict/no-merge and latest-state/no-FIFO behaviour.

The next engineering gate is:

```text
DEV-011 — static MQTT-only Web UI
```

## DEV-010 result

### Architecture now Confirmed

Real Art-Net flow:

```text
IPv4 UDP 6454
-> LinuxArtNetDatagramTransport
-> ArtNetRuntime
-> ArtNetCore
-> latest immutable Art-Net physical snapshot
-> ArtNetSourceCoordinator
-> DmxSourceRouter
-> DmxOutput mailbox
-> fixed 44 Hz physical DMX
```

Important invariants:

- Art-Net receiver remains alive regardless of selected application Source;
- MQTT and Art-Net continue updating independently;
- explicit `mqtt | artnet` selector is the only application source switch;
- network loss never automatically switches application Source;
- only whole snapshots reach `DmxOutput`;
- physical source changes occur at DMX frame boundaries;
- physical DMX cadence remains fixed at 44 Hz;
- ArtDmx arrival never directly starts UART transmission;
- latest committed Art-Net state is sampled, not replayed from a FIFO;
- network state keeps 512 channels while the physical product profile uses
  channels `1..300`;
- before the first valid process-local ArtDmx, selecting ART-NET does not invent a
  zero/blackout frame;
- LOST holds the last committed Art-Net state and releases the stale source lock;
- source identity is `IPv4 + ArtDmx.Physical`;
- a conflicting source does not merge into or mutate the active source state.

### UDP runtime / discovery

Confirmed:

```text
UDP port:               6454
protocol revision:      >= 14
Port-Address:           0..32767
DMXWB compatibility:    Port-Address 0 explicitly supported
PollReply RefreshRate:  44
PollReply delay:        randomized 0..1 s
PollReply destination:  unicast to poll sender
```

ArtPoll/ArtPollReply subscription remains advertised while application
`Source=MQTT`. GoodOutput/data-active reflects actual selected Art-Net physical
output rather than subscription existence.

The runtime can close/rebind/recover the UDP socket without restarting the
application process.

### Source selector

`DmxSourceRouter` caches the latest whole snapshot from each source.

```text
MQTT -> ART-NET
```

- if an Art-Net snapshot exists, the next whole physical publication uses it;
- if no valid ArtDmx has existed since process start, current physical output is
  preserved until one arrives.

```text
ART-NET -> MQTT
```

- the current whole logical MQTT snapshot becomes physical;
- MQTT logical state has continued updating while ART-NET was selected.

GoodOutput active state becomes true only after an Art-Net snapshot is
successfully published through the physical source path.

## DEV-010 acceptance matrix

External controller used for interoperability:

```text
QLC+ 5.2.2
```

Primary acceptance WB8:

```text
Controller:       Wiren Board rev. 8.5.1 (T507)
Architecture:     aarch64 / arm64
OS:               Debian 11 Bullseye
WB release:       wb-2606 stable
Kernel:           6.8.0-wb160
glibc:            2.31
DMX port:         /dev/ttyRS485-1 -> ttyS2
MQTT broker:      127.0.0.1:1883
```

### DEV-010A — real QLC+ networking

Report:

```text
docs/DEV010A_ARTNET_QLCPLUS_NETWORK_REPORT.txt
=== DMXWB DEV-010A QLC+ ART-NET NETWORK PASS ===
```

Confirmed:

- DMXWB node discovery in QLC+ Nodes Tree;
- randomized unicast PollReply traffic;
- real ArtDmx reception;
- ACTIVE -> LOST after the 3 s timeout;
- Hold Last during source loss;
- same-runtime recovery after ArtDmx returns;
- no core/network errors in the accepted run.

### DEV-010B — physical MQTT / ART-NET integration

Source switching report:

```text
docs/DEV010B_ARTNET_MQTT_SOURCE_SWITCH_REPORT.txt
=== DMXWB DEV-010B QLC+ PHYSICAL SOURCE SWITCH PASS ===
```

Confirmed physically:

```text
MQTT red
-> background Art-Net green does not take over
-> explicit MQTT -> ART-NET gives green
-> background MQTT blue does not take over
-> explicit ART-NET -> MQTT gives current blue
```

LOST/Hold/recovery report:

```text
docs/DEV010B_ARTNET_LOST_HOLD_RECOVERY_REPORT.txt
=== DMXWB DEV-010B ART-NET LOST HOLD RECOVERY PASS ===
```

Confirmed:

- `Source=artnet` remains selected after Art-Net loss;
- physical output Holds Last;
- 44 Hz transmission continues;
- same process accepts the returning source;
- no operator restart is needed.

### DEV-010C1 — WB interface down/up

Report:

```text
docs/DEV010C1_WB8_INTERFACE_RECOVERY_REPORT.txt
=== DMXWB DEV-010C1 WB8 INTERFACE DOWN UP RECOVERY PASS ===
```

Real WB network interface down/up was exercised while the acceptance runtime and
physical DMX continued. The same PID/start time recovered after the interface
returned and accepted new ArtDmx without application restart.

### DEV-010C2 — controller restart

Report:

```text
docs/DEV010C2_QLCPLUS_RESTART_RECOVERY_REPORT.txt
=== DMXWB DEV-010C2 QLC+ CONTROLLER RESTART RECOVERY PASS ===
```

QLC+ 5.2.2 was fully closed for longer than the source timeout and restarted on
the same IP. Physical Hold Last continued during the outage and new ArtDmx was
accepted by the same DMXWB process after QLC+ returned.

### DEV-010C3 — real source IPv4 change

Report:

```text
docs/DEV010C3_ARTNET_SOURCE_IP_CHANGE_REPORT.txt
=== DMXWB DEV-010C3 REAL-NETWORK SOURCE IPv4 CHANGE PASS ===
```

Two real network paths were present simultaneously:

```text
primary:
QLC+ 10.200.200.2 -> WB8 10.200.200.1

Wi-Fi:
QLC+ 192.168.42.160 -> WB8 192.168.42.1
```

Acceptance sequence:

```text
primary GREEN
-> primary output disabled
-> >3 s LOST / Hold Last GREEN
-> stale source lock released
-> same QLC+ reconfigured to Wi-Fi
-> Wi-Fi RED accepted
```

Confirmed in the same DMXWB process:

- Windows selected the intended source IP for each destination;
- WB8 used distinct `dbg0` and `wlan0` paths;
- application Source stayed ART-NET;
- source IPv4 changed from `10.200.200.2` to `192.168.42.160`;
- physical output changed GREEN -> RED without restart;
- `final_artnet_conflicts = 0`;
- `final_dmx_missed_deadlines = 0`;
- physical refresh remained 44 Hz.

### DEV-010C4 — source conflict / no merge

Report:

```text
docs/DEV010C4_ARTNET_CONFLICT_REPORT.txt
=== DMXWB DEV-010C4 ART-NET SOURCE CONFLICT NO-MERGE PASS ===
```

A conflicting source identity was injected while QLC+ remained active.

Confirmed:

- 150 conflicting ArtDmx packets were detected;
- active physical output was not taken over or merged;
- committed active-source state remained stable;
- same runtime continued;
- physical output remained fixed 44 Hz.

### DEV-010C5 — real UDP ArtSync

Report:

```text
docs/DEV010C5_ARTSYNC_REPORT.txt
=== DMXWB DEV-010C5 REAL UDP ARTSYNC STAGING ATOMIC RELEASE PASS ===
```

Observed sequence:

```text
first ArtSync
-> RED ArtDmx staged for 2530 ms
-> fixture remains GREEN
-> second ArtSync
-> one physical transition to RED
```

Accepted run:

```text
red_staging_packets: 51
staging_window_ms:    2530
dmx_missed_deadlines: 0
active_refresh_hz:    44
software_result:      PASS
```

This proves real UDP ArtSync staging/release without moving the physical 44 Hz
scheduler.

### DEV-010C6 — repeated reconnect and latest/no-FIFO

Report:

```text
docs/DEV010C6_RECONNECT_LATEST_REPORT.txt
=== DMXWB DEV-010C6 REPEATED RECONNECT LATEST NO-FIFO PASS ===
```

Three sequential reconnect cycles proved Hold Last and same-process reacquisition.

Burst stress:

```text
Art-Net datagrams received:       4404
committed snapshots published:    4396
router Art-Net snapshots sampled: 449
burst packets requested/sent:     4096
burst duration:                   473 ms
```

The final BLUE guard became stable without multi-second playback of earlier burst
states. This confirms the intended latest-state/coalescing architecture rather
than an ArtDmx FIFO.

Final accepted physical diagnostics included:

```text
dmx_frames_sent:       4234
dmx_missed_deadlines:  0
dmx_active_refresh_hz: 44
receive_errors:        0
send_errors:           0
core_rejections:       0
conflicts:             0
software_result:       PASS
```

## DEV-010 roadmap conclusion

The DEV-010 acceptance requirements are now Confirmed:

- named/versioned external Art-Net controller/tool;
- discovery/subscription;
- real ArtDmx;
- ArtSync;
- Ethernet/source loss durations and Hold Last;
- controller restart;
- real source IPv4 change;
- WB interface down/up;
- repeated reconnect;
- IP+Physical conflict;
- no operator restart after temporary network failure;
- no accumulating ArtDmx FIFO latency;
- fixed physical 44 Hz;
- latest committed channels `1..300`.

Therefore DEV-010 receives engineering PASS.

## Production items still Deferred

DEV-010 engineering PASS does **not** claim production release readiness.

Still Deferred to the appropriate later gates:

- registered production Art-Net OEM Code;
- production packaging/systemd/offline installation;
- final integrated 24-hour acceptance.

Development acceptance uses an explicitly marked non-production OEM placeholder
only. No production OEM Code is invented.

## Confirmed baseline retained from earlier gates

Physical DMX profile remains:

```text
kDmxMaxChannels       = 512
kDmxPhysicalMaxSlots  = 300
kDmxOutputRefreshHz   = 44
```

The previously Confirmed Fixture/Group/Scene, persistence and MQTT behaviour
remains unchanged by DEV-010.

MQTT canonical flow remains:

```text
disk
-> C++ canonical model
-> retained MQTT representation
```

MQTT callback remains parse/enqueue only and does not perform serial or persistence
I/O.

## Current engineering gate

```text
DEV-011 — static MQTT-only Web UI
```

DEV-011 must preserve all Confirmed physical/source/network invariants. In
particular, browser/web/MQTT work must not become part of the physical DMX timing
path.

## Build/test policy

```text
Windows host                 -> project files / ZIP / Git / WSL launch
Local Linux / WSL on laptop -> C++ build/tests + target build
Bullseye ARM64 toolchain     -> WB8 target artifact
WB8                          -> runtime/hardware/integration acceptance
Docker                       -> not used
```

Windows/MSVC is not part of the supported build/test matrix.

## Next

```text
DEV-011 — static MQTT-only Web UI
```
