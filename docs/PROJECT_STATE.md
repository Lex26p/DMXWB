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


## DEV-012 execution plan — Decided before implementation

DEV-012 is intentionally split before implementation. The gate combines production
runtime consolidation, operational diagnostics, systemd lifecycle, artifact
closure, installer work and real offline WB8 acceptance; treating all of that as
one handoff would make scope control and failure isolation poor.

The planned order is fixed unless new factual evidence requires an explicit plan
revision under `AGENTS.md`.

### DEV-012A — production daemon consolidation

Scope:

- make the real `dmxwb` target the integrated daemon rather than the historical
  DEV-003/004/005 diagnostic CLI;
- extract/reuse the already Confirmed MQTT + Art-Net + source router + DmxOutput +
  persistence orchestration instead of maintaining a second production runtime;
- preserve dynamic DMX Port and Art-Net Port-Address reconfiguration proved in
  DEV-011F4B;
- default production paths:
  `/etc/dmxwb/config.json` and `/var/lib/dmxwb/state.json`;
- preserve graceful signal handling and whole-snapshot/44 Hz invariants;
- keep development/acceptance diagnostics outside the production runtime path.

PASS:

- native Linux build/tests PASS;
- Bullseye ARM64 `dmxwb` is a real integrated target artifact and dynamically
  requires the required MQTT runtime;
- foreground WB8 smoke proves MQTT + physical DMX + Art-Net through the production
  runtime path without systemd yet;
- no duplicated independent integrated runtime remains as the production design.

### DEV-012B — production diagnostics, logging and WB UI contract

Scope:

- implement only diagnostics required by `TECHNICAL_SPEC.md` section 20;
- keep `/dmxwb/status` as the single structured diagnostic snapshot API;
- no telemetry database, metrics server or SCADA/dashboard subsystem;
- expose required DMX/MQTT/Art-Net/configuration state and recovery/error fields;
- update status at a bounded low rate/event-driven cadence, never from the DMX
  timing path;
- stdout/stderr event logging suitable for journald;
- log startup/shutdown, config load/save/reject, serial lost/recovered, MQTT
  connect/lost/recovered, Art-Net source events, Scene apply and fatal errors;
- do not log every DMX frame or slider movement;
- preserve MQTT LWT `off`;
- hide Fixture/Group/Scene devices/controls from standard WB HomeUI so only DMXWB
  Status/Source is visible there.

PASS:

- host contract/tests PASS;
- real WB8 retained status contains the required structured subsystem information;
- standard WB UI shows only the intended DMXWB Status/Source surface;
- browser Web remains a compact control/configuration UI, not a monitoring
  dashboard.

### DEV-012C — systemd lifecycle

Scope:

- add `deploy/dmxwb.service`;
- `Type=simple`;
- `Restart=on-failure`;
- `RestartSec=2s`;
- service must not require Mosquitto as a precondition for the DMX process to
  start;
- graceful SIGTERM flushes dirty state and closes MQTT/UDP/serial cleanly;
- recoverable MQTT/Art-Net/serial/network failures remain in-process recovery;
- systemd restart is reserved for real process exit/failure.

PASS on WB8:

- start/stop/restart PASS;
- clean stop does not trigger an unwanted restart;
- deliberate process crash is recovered by systemd;
- MQTT broker restart does not restart the DMXWB process;
- recoverable serial/network events do not require manual/systemd restart;
- journald contains the expected bounded lifecycle/recovery events.

### DEV-012D — reproducible production artifact and offline bundle closure

Scope:

- reproduce the production AArch64 binary on the laptop with the confirmed
  Bullseye toolchain;
- verify architecture, GLIBC compatibility and dynamic dependency closure;
- determine which runtime libraries are guaranteed by the tested WB image and
  which, if any, must be supplied locally in the bundle;
- create a deterministic bundle layout containing the production binary, static
  Web, systemd unit, installer, example/default config and required local runtime
  files;
- record artifact/toolchain identity and checksums;
- no source compilation on WB8.

Art-Net production identity checkpoint:

- a registered production Art-Net OEM Code is required before a bundle may be
  called production-ready;
- a development placeholder may still be used by development acceptance tools,
  but it must never be silently promoted into the production bundle;
- if the registered OEM Code is still unavailable at this point, DEV-012 stops at
  this checkpoint rather than inventing one.

PASS:

- repeated laptop build/bundle creation is reproducible enough to identify the
  same inputs/artifacts;
- bundle dependency audit is complete for the tested WB8 software image;
- production bundle contains no development OEM placeholder.

### DEV-012E — offline installer and filesystem/permission contract

Scope:

- add `deploy/install_wirenboard.sh`;
- install only from local bundle contents;
- create/use `/etc/dmxwb`, `/var/lib/dmxwb`, `/var/www/dmxwb`;
- install production binary and `dmxwb.service`;
- apply required permissions;
- preserve an existing valid config/state rather than silently overwriting user
  data;
- fail clearly if required stock WB components are absent;
- enable/start the service as required by the installation flow;
- do not add an uninstall/update framework unless a later requirement explicitly
  needs one.

Installer must not execute or require:

```text
apt update
online apt install
git clone
curl/wget downloads
npm/Node frontend build
compiler/CMake installation
DMXWB source compilation on WB8
Docker
```

PASS:

- static installer audit PASS;
- repeat install is safe for existing config/state;
- local bundle contains everything the tested target needs beyond guaranteed stock
  WB components;
- no network-download path exists.

### DEV-012F — real offline WB8 install/reboot acceptance

Scope:

- perform the roadmap's clean installation with external Internet physically
  unavailable while keeping the required local LAN;
- install only from the prepared local bundle;
- record WB8 model/software version plus binary/toolchain/bundle identity;
- verify service start and full WB8 reboot;
- verify config/state restore;
- verify local Mosquitto/reconnect;
- verify Web after reboot;
- verify basic physical DMX;
- verify basic Art-Net input;
- verify crash -> systemd recovery;
- verify broker restart and serial recovery without application restart;
- verify standard WB UI is not polluted by Fixture/Group/Scene devices;
- verify target does not need compiler/CMake/Node/Docker.

PASS:

- clean offline install, reboot and basic operation all PASS;
- no external download or source compilation occurs on WB8;
- recoverable subsystem failures recover in-process;
- systemd is needed only for a real process failure.

The 24-hour test and full cross-subsystem final acceptance remain DEV-013 and are
not pulled into DEV-012.

### DEV-012G — gate closeout documentation

Scope after DEV-012F PASS:

- update `docs/PROJECT_STATE.md` with the accepted offline installation
  configuration, artifact/toolchain identity and DEV-012 PASS;
- update `README.md` with the actual production build/install/run commands;
- update reusable reference documentation only if DEV-012 produced portable
  deployment knowledge worth preserving;
- do not change `TECHNICAL_SPEC.md` or `ROADMAP.md` unless the requirements or
  roadmap genuinely changed.

PASS:

- documentation matches the accepted production installation;
- current gate advances to DEV-013;
- no new engineering functionality is introduced in the closeout commit.


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
