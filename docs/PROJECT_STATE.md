# PROJECT_STATE

**Last updated:** 2026-08-28

## Repository / acceptance base

```text
d356b41a99572daaaa58126244b84577ddd449ea
Complete DEV-011 minimal status contract
```

## Last confirmed engineering PASS

```text
DEV-011 — static MQTT-only Web UI
```

DEV-011 is **Confirmed** by static/host checks and real WB8 browser/MQTT/physical
acceptance. The Web remains a static MQTT-only client and does not enter the DMX
timing path.

The next engineering gate is:

```text
DEV-012 — systemd, diagnostics and fully offline deployment
```

## Current confirmed product architecture

```text
Browser
/var/www/dmxwb
    |
MQTT WebSocket /mqtt
    |
Mosquitto
    |
    +---------------- WB MQTT logical model ----------------+
    |                                                       |
Fixture / Group / Scene                              Art-Net UDP 6454
    |                                                       |
mqtt whole snapshot                                  ArtNetRuntime
    |                                                       |
    +---------------------- DmxSourceRouter ----------------+
                               |
                         mqtt | artnet
                               |
                           DmxOutput
                         fixed 44 Hz
                               |
                      /dev/ttyRS485-*
                               |
                             DMX512
```

Core invariants remain Confirmed:

- one physical DMX output;
- only `DmxOutput` owns the serial port;
- MQTT, Web and Art-Net never write directly to serial;
- physical output uses only the explicitly selected `mqtt | artnet` source;
- no automatic source switching;
- MQTT and Art-Net continue updating independently in the background;
- only whole snapshots reach the physical DMX path;
- source changes occur at DMX frame boundaries;
- physical output remains fixed at 44 Hz;
- Art-Net uses latest committed state rather than a FIFO;
- Art-Net LOST keeps the selected Source and Holds Last;
- Web is not required for DMX to continue after the browser is closed.

## DEV-011 result

### Static Web

Confirmed production Web layout:

```text
www/dmxwb/
    index.html
    app.js
    model.js
    mqtt-client.js
    styles.css
```

Confirmed:

- HTML/CSS/vanilla JavaScript only;
- no Node.js runtime;
- no npm/build step;
- no external Internet dependencies;
- served from `/dmxwb/`;
- MQTT WebSocket `/mqtt`;
- current page hostname is used for the WB connection;
- Web has no direct serial/file/systemd API;
- user-facing interface is Russian;
- `DMX`, `MQTT`, `Art-Net`, `WB MQTT`, `ART-NET` and device paths remain technical
  names rather than translated protocol identifiers.

### Browser MQTT/reconnect

Report:

```text
docs/DEV011B2_MQTT_WEBSOCKET_REPORT.txt
```

Confirmed on WB8:

- real browser WebSocket connection to local Mosquitto;
- disconnect is shown to the user;
- new commands are blocked while disconnected;
- reconnect is automatic;
- retained config/state/status are re-subscribed;
- old commands are not replayed after reconnect;
- physical DMX does not depend on the open browser.

### Fixture and Source

Report:

```text
docs/DEV011C2_WEB_FIXTURE_SOURCE_REPORT.txt
```

Confirmed through real browser -> MQTT -> application -> physical DMX:

- Fixture Power and RGB controls;
- slider throttle/final publish contract;
- explicit `WB MQTT / ART-NET` Source switching;
- background MQTT logical state remains current while ART-NET is selected;
- switching back to MQTT uses the latest whole logical MQTT state.

### Groups

Report:

```text
docs/DEV011D2_WEB_GROUP_REPORT.txt
```

Confirmed:

- Group controls use MQTT only;
- exact Group subscriptions are derived from canonical config;
- retained subscription loops are avoided;
- Group state is factually confirmed from backend MQTT state;
- real grouped physical DMX control passed on WB8.

### Scenes

Report:

```text
docs/DEV011E2_WEB_SCENE_REPORT.txt
```

Confirmed:

- create;
- apply;
- overwrite;
- rename;
- delete;
- request/result matching;
- Scene Apply produces one whole post-mutation snapshot rather than visible
  per-Fixture iteration;
- revision progressed monotonically through lifecycle operations.

### Structural configuration transaction

Report:

```text
docs/DEV011F3_WEB_CONFIG_TRANSACTION_REPORT.txt
```

Confirmed:

- local structural draft;
- explicit `Применить`;
- full `/dmxwb/config/set` proposal;
- `expected_revision`;
- two-tab stale revision conflict rejection;
- invalid config rejection;
- stable monotonic IDs;
- Fixture removal cleans Group membership without rewriting historical Scene
  snapshots;
- canonical retained config is republished after successful apply.

### Runtime structural transport apply

Report:

```text
docs/DEV011F4B_TRANSPORT_STRUCTURAL_APPLY_REPORT.txt
=== DMXWB DEV-011F4B REAL DMX PORT + ART-NET UNIVERSE APPLY PASS ===
```

Confirmed in one WB8 process/PID:

```text
DMX Port:
  /dev/ttyRS485-1
  -> /dev/ttyRS485-2
  -> /dev/ttyRS485-1

Art-Net Port-Address:
  0
  -> 17
  -> 0
```

The accepted run proved:

- two successful DMX-port runtime reconfigurations;
- zero DMX-port reconfiguration failures;
- two successful Art-Net universe reconfigurations;
- zero Art-Net universe reconfiguration failures;
- old Art-Net universe data is not replayed after reconfiguration;
- universe change preserves Hold Last and does not create a blackout;
- Source switching continues to use whole current snapshots;
- the same runtime PID survives all reconfiguration;
- physical DMX remains 44 Hz;
- final `software_result: PASS`.

### Minimal Web status contract

Retained `/dmxwb/status` now contains the required top-level fields:

```text
application
dmx
mqtt
artnet
configuration
last_error
```

DEV-011 deliberately keeps this status **minimal**. Extended counters, telemetry,
monitoring and deployment/service diagnostics are not part of the Web gate and are
Deferred to DEV-012 where diagnostics belong in the roadmap.

## DEV-011 acceptance conclusion

The DEV-011 roadmap goal is Confirmed:

- static MQTT-only Web is implemented;
- no direct browser access to serial/files/systemd exists;
- Fixture/Group/Scene/Source management works through MQTT;
- MQTT reconnect works without replaying old commands;
- revision conflict and invalid structural config are rejected;
- structural DMX Port and Art-Net Universe changes are applied by the running
  process;
- browser closure does not stop physical DMX;
- previously Confirmed DMX and Art-Net invariants remain intact.

Therefore DEV-011 receives engineering PASS.

## Confirmed target / hardware baseline

Primary acceptance WB8:

```text
Controller:       Wiren Board rev. 8.5.1 (T507)
Architecture:     aarch64 / arm64
OS:               Debian 11 Bullseye
WB release:       wb-2606 stable
Kernel:           6.8.0-wb160
glibc:            2.31
DMX ports:        /dev/ttyRS485-1 and /dev/ttyRS485-2
MQTT broker:      127.0.0.1:1883
Web:              nginx + Mosquitto WebSocket /mqtt
```

Physical product profile remains:

```text
kDmxMaxChannels       = 512
kDmxPhysicalMaxSlots  = 300
kDmxOutputRefreshHz   = 44
```

## Production items still Deferred

DEV-011 PASS does **not** claim production release readiness.

Still Deferred:

- production systemd service and installer;
- reproducible fully offline installation bundle;
- deployment permissions/layout and reboot acceptance;
- production logging and diagnostics;
- standard WB UI cleanup to only the intended system Status/Source surface;
- registered production Art-Net OEM Code;
- final integrated/offline/24-hour acceptance.

## Current engineering gate

```text
DEV-012 — systemd, diagnostics and fully offline deployment
```

DEV-012 must package the already Confirmed application as a normal WB8 daemon and
prove installation/reboot/basic operation with no external Internet and no source
compilation on WB8.

## Build/test policy

```text
Windows host                 -> project files / ZIP / Git / WSL launch
Local Linux / WSL on laptop -> C++ build/tests + target build
Bullseye ARM64 toolchain     -> WB8 target artifact
WB8                          -> runtime/hardware/integration acceptance
Docker                       -> not used
```

Windows/MSVC is not part of the supported build/test matrix.
