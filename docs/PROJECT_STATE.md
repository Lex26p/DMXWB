# PROJECT_STATE

**Last updated:** 2026-08-31

## Repository source base

```text
0c4e9f2ea0c06785d0a3ce5c714d46a0ef29ab4d
Complete DEV-012B systemd diagnostics
```

## Last confirmed engineering PASS

```text
DEV-012B — systemd and essential operational diagnostics
```

DEV-012B is **Confirmed** by the static operational contract, native Linux tests,
Bullseye ARM64 artifact checks and real WB8 systemd/recovery acceptance using the
production `dmxwb` executable. DEV-012A remains the Confirmed production foreground
baseline; DEV-011 remains the Confirmed functional Web/MQTT baseline.

## Current engineering gate

```text
DEV-012 — systemd, diagnostics and fully offline deployment
```

DEV-012 packages the already Confirmed application as a normal WB8 daemon and
proves installation, reboot and basic operation without external Internet and
without source compilation on WB8.

The current active step is:

```text
DEV-012B1 — production diagnostics contract correction
```

Before DEV-012C, DEV-012 has a fixed corrective sequence for separating
engineering/test instrumentation from production operational diagnostics.
DEV-012B remains Confirmed for its proven systemd lifecycle and recovery behavior;
the corrective sequence narrows production observability without invalidating that
hardware/service acceptance.

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

Core invariants are **Confirmed**:

- one physical DMX output;
- only `DmxOutput` owns the serial port;
- MQTT, Web and Art-Net never write directly to serial;
- physical output uses only the explicitly selected `mqtt | artnet` source;
- no automatic source switching;
- MQTT and Art-Net continue updating independently in the background;
- only whole snapshots reach the physical DMX path;
- source changes occur at DMX frame boundaries;
- physical output remains fixed at 44 Hz;
- physical projection is limited to channels 1..300;
- internal DMX/Art-Net state remains 512 channels;
- Art-Net uses latest committed state rather than a FIFO;
- Art-Net LOST keeps the selected Source and Holds Last;
- Web is not required for DMX to continue after the browser is closed.

## Confirmed DEV-011 baseline

Production Web layout:

```text
www/dmxwb/
    index.html
    app.js
    model.js
    mqtt-client.js
    styles.css
```

Confirmed properties:

- HTML/CSS/vanilla JavaScript only;
- no Node.js runtime or npm/build step;
- no external Internet dependencies;
- served from `/dmxwb/`;
- MQTT WebSocket `/mqtt`;
- current page hostname is used for the WB connection;
- Web has no direct serial/file/systemd API;
- user-facing interface is Russian;
- Fixture/Group/Scene/Source management works only through MQTT;
- browser reconnect does not replay stale commands;
- browser closure does not stop physical DMX;
- structural configuration uses explicit apply and revision conflict protection;
- DMX Port and Art-Net Port-Address can be applied by the running process without
  restart.

DEV-011 acceptance reports:

```text
docs/DEV011B2_MQTT_WEBSOCKET_REPORT.txt
docs/DEV011C2_WEB_FIXTURE_SOURCE_REPORT.txt
docs/DEV011D2_WEB_GROUP_REPORT.txt
docs/DEV011E2_WEB_SCENE_REPORT.txt
docs/DEV011F3_WEB_CONFIG_TRANSACTION_REPORT.txt
docs/DEV011F4B_TRANSPORT_STRUCTURAL_APPLY_REPORT.txt
```

Runtime structural transport apply was Confirmed in one WB8 process/PID:

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

The accepted run proved successful runtime reconfiguration without violating the
fixed 44 Hz physical output contract.

Retained `/dmxwb/status` currently has the required top-level structure:

```text
application
dmx
mqtt
artnet
configuration
last_error
```

DEV-012 may add only operational fields that are actually needed for production
status and recovery diagnostics. It must not introduce a telemetry database,
metrics server, SCADA subsystem or monitoring dashboard.

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

Physical product profile:

```text
kDmxMaxChannels       = 512
kDmxPhysicalMaxSlots  = 300
kDmxOutputRefreshHz   = 44
```

## Current production runtime implementation

The production executable target is now `dmxwb`.

The historical DEV-003/004/005 diagnostic CLI remains a separate engineering
executable and is not the production runtime path.

Current production flow:

```text
/etc/dmxwb/config.json
/var/lib/dmxwb/state.json
          |
          v
   IntegratedRuntime
      |        |
     MQTT    Art-Net
      \        /
     DmxSourceRouter
          |
       DmxOutput
          |
        RS-485
```

`IntegratedRuntime` owns the shared production orchestration for:

- persistence;
- MQTT runtime;
- Art-Net runtime;
- explicit source routing;
- physical `DmxOutput`;
- runtime DMX Port reconfiguration;
- runtime Art-Net Port-Address reconfiguration;
- graceful shutdown and state flush.

Production `dmxwb` currently keeps ArtDmx input active without inventing an
unregistered Art-Net OEM identity. Production ArtPollReply advertisement must use
a registered OEM Code when that identity is available.

## DEV-012A acceptance result

DEV-012A is **Confirmed**.

Implementation base:

```text
bdb88d9f3ef4ec21785ae14de5cd45208a527905
Complete production daemon consolidation
```

Acceptance source head:

```text
e850cee8633b2d18c89b63869524a2408dcfc5ef
```

Foreground WB8 report:

```text
docs/DEV012A_PRODUCTION_FOREGROUND_REPORT.txt
=== DMXWB DEV-012A PRODUCTION FOREGROUND PASS ===
```

Confirmed build/toolchain facts:

- native Linux build with warnings-as-errors PASS;
- `16/16` CTest tests PASS;
- Bullseye GCC `10.2.1` AArch64 production build PASS;
- Bullseye ARM64 `libmosquitto` version `2.0.11`;
- production `dmxwb` is ELF AArch64;
- maximum required GLIBC symbol version is `GLIBC_2.17`;
- production `dmxwb` dynamically requires `libmosquitto.so.1`;
- production artifact SHA256:
  `193f44b037a72dbc26f64c04c8720b841838690714d5f2c7f47ab89589ad6e91`.

Confirmed real WB8 foreground path through the production executable:

```text
production dmxwb startup
    -> MQTT Fixture RED -> physical RED
    -> Source ART-NET + ArtDmx BLUE -> physical BLUE
    -> inactive MQTT state updated to GREEN while physical remains BLUE
    -> Source WB MQTT -> latest GREEN becomes physical
    -> final Power OFF
    -> SIGTERM
    -> persistent state flush
    -> serial port released
```

The acceptance helper preserved and restored the retained MQTT topics it touched.
No systemd service was involved; service lifecycle belongs to DEV-012B.

## DEV-012B acceptance result

DEV-012B is **Confirmed**.

Acceptance source base:

```text
eb5c9030d31b09d5ea2c46ca0dc1052c3426168d
Confirm DEV-012A production daemon acceptance
```

The DEV-012B implementation was tested from the modified worktree based on that
commit and is intended to be committed together with this acceptance record.

WB8 report:

```text
docs/DEV012B_SYSTEMD_REPORT.txt
=== DMXWB DEV-012B SYSTEMD + DIAGNOSTICS PASS ===
```

Confirmed build/toolchain facts:

- DEV-012B static operational contract PASS;
- native Linux build with warnings-as-errors PASS;
- `16/16` CTest tests PASS;
- Bullseye GCC `10.2.1` AArch64 production build PASS;
- production `dmxwb` is ELF AArch64;
- maximum required GLIBC symbol version is `GLIBC_2.17`;
- production `dmxwb` dynamically requires `libmosquitto.so.1`;
- production artifact SHA256:
  `55278d3bca2f1a262cecf29608480bde2e89a238601d9f3b4aaaf8dfa65b20b9`.

Confirmed real WB8 service/recovery path:

```text
systemctl start
    -> production dmxwb active
    -> retained operational /dmxwb/status
    -> MQTT RED -> physical RED

clean systemctl stop
    -> no unwanted restart
    -> persistent state flush

second start
    -> saved RED restored

systemctl restart
    -> new process PID

SIGKILL production process
    -> systemd automatic recovery
    -> new process PID
    -> physical RED restored

restart Mosquitto
    -> DMXWB PID unchanged
    -> MQTT reconnects in-process
    -> physical DMX remains stable

journald
    -> bounded lifecycle/recovery events

standard WB HomeUI
    -> only intended DMXWB Status/Source surface

final Power OFF
    -> clean service stop
    -> original WB8 environment restored
```

The WB8 run additionally Confirmed MQTT recovery diagnostics with one disconnect and
two successful connections while the DMXWB process PID remained unchanged.

DEV-012B introduced no separate metrics server, telemetry database, monitoring
service or dashboard, and Fixture/Group/Scene controls remain hidden from standard
WB HomeUI. A post-PASS audit of the production observability path found that
cumulative engineering/test counters are still accumulated and exposed by the
production runtime. This does not invalidate the Confirmed systemd/recovery behavior,
but it must be corrected before DEV-012C. The fixed corrective sequence is
DEV-012B1 -> DEV-012B2 -> DEV-012B3.

## DEV-012 execution plan

### DEV-012A — production daemon and foreground acceptance — Confirmed

Scope:

- use the real `dmxwb` target as the integrated production daemon;
- use one shared `IntegratedRuntime` for persistence, MQTT, Art-Net,
  `DmxSourceRouter` and `DmxOutput`;
- preserve runtime DMX Port and Art-Net Port-Address reconfiguration;
- use default production paths:
  `/etc/dmxwb/config.json` and `/var/lib/dmxwb/state.json`;
- preserve graceful SIGINT/SIGTERM handling and dirty-state flush;
- keep engineering diagnostics outside the production runtime path;
- preserve whole-snapshot publication and fixed 44 Hz physical output.

PASS:

- native Linux build/tests PASS;
- Bullseye ARM64 production `dmxwb` build PASS;
- target artifact architecture/GLIBC/dependency checks PASS;
- production `dmxwb` dynamically uses the required MQTT runtime;
- foreground WB8 smoke proves:
  - daemon startup;
  - MQTT connection;
  - MQTT Fixture command -> physical DMX;
  - Art-Net -> physical DMX;
  - explicit Source switching;
  - clean SIGINT/SIGTERM shutdown and state flush;
- no independent duplicate production runtime remains.

### DEV-012B — systemd and essential operational diagnostics — Confirmed

Scope:

- add `deploy/dmxwb.service`;
- `Type=simple`;
- `Restart=on-failure`;
- `RestartSec=2s`;
- service starts the production `dmxwb` daemon;
- Mosquitto is not a hard startup precondition for the DMXWB process;
- graceful SIGTERM closes MQTT/UDP/serial and flushes dirty state;
- recoverable MQTT/Art-Net/serial/network failures remain in-process recovery;
- systemd restart is reserved for real process exit/failure;
- stdout/stderr event logging is suitable for journald;
- log only meaningful lifecycle/recovery/error events;
- keep `/dmxwb/status` as the structured application status;
- expose only operational state needed for DMX, MQTT, Art-Net, configuration and
  last error;
- keep Fixture/Group/Scene hidden from standard WB HomeUI so only the intended
  DMXWB Status/Source surface is visible there.

No separate metrics server, telemetry database, monitoring service or dashboard is
introduced.

PASS on WB8:

- `systemctl start dmxwb` PASS;
- `systemctl stop dmxwb` PASS;
- `systemctl restart dmxwb` PASS;
- clean stop does not cause an unwanted restart;
- deliberate process failure is recovered by systemd;
- Mosquitto restart does not restart the DMXWB process;
- MQTT reconnects in-process;
- required retained status is present and factual;
- journald contains bounded startup/shutdown/recovery/error events;
- standard WB HomeUI exposes only the intended DMXWB Status/Source surface.

### DEV-012B1 — production diagnostics contract correction

Scope:

- update `docs/TECHNICAL_SPEC.md` so production diagnostics and engineering/test
  instrumentation are explicitly separate concepts;
- production `dmxwb` must expose factual current state, current configuration,
  current source, last error and recovery state only where operationally useful;
- cumulative test/acceptance counters such as lifetime frame, packet, command,
  publication, snapshot and source-switch totals are not part of the production
  contract;
- preserve algorithmic revisions/generations, protocol sequence state, config
  revision and stable Fixture/Group/Scene ID generators because they are runtime
  state rather than telemetry;
- do not change C++ implementation in B1.

PASS:

- `TECHNICAL_SPEC.md` no longer requires cumulative engineering counters in
  production;
- allowed production operational state is explicit;
- required algorithmic revisions/generations/IDs remain required;
- documentation has no contradictory production counter requirement;
- active step advances to DEV-012B2.

### DEV-012B2 — production / engineering instrumentation separation

Scope:

- preserve engineering counters required by unit/integration/acceptance tests;
- build production `dmxwb` without accumulating test-only counters;
- remove test-only counter updates from production DMX, Art-Net, MQTT, router and
  coordinator paths, especially from the physical DMX hot path;
- do not duplicate DMX/MQTT/Art-Net algorithms to achieve the separation;
- make `/dmxwb/status` factual state-oriented rather than lifetime-statistics
  oriented;
- derive production recovery logging from state transitions, not historical counter
  deltas;
- update static/build/acceptance checks to the corrected contract;
- add a build-level assertion that the production status contract does not expose
  forbidden cumulative telemetry fields.

PASS:

- native Linux warnings-as-errors build PASS;
- all existing host tests PASS with engineering instrumentation available;
- production build does not accumulate the forbidden test-only counters;
- production `/dmxwb/status` contains no forbidden cumulative telemetry fields;
- Bullseye ARM64 build and architecture/GLIBC/dependency audit PASS;
- active step advances to DEV-012B3.

### DEV-012B3 — WB8 regression after counter isolation

Scope:

- run the corrected production `dmxwb` on the real WB8 through systemd;
- verify MQTT -> physical DMX, Art-Net -> physical DMX and explicit Source
  switching;
- restart Mosquitto and verify in-process MQTT recovery without changing the
  `dmxwb` PID and without interrupting physical DMX;
- verify `/dmxwb/status` remains factual and contains no forbidden cumulative
  counters;
- verify journald contains bounded lifecycle/error/recovery transition events rather
  than telemetry streaming;
- verify clean stop, state flush and serial release.

PASS:

- production behavior proven by DEV-012A/DEV-012B remains intact;
- MQTT broker recovery remains in-process;
- physical DMX remains continuous through the recovery check;
- production status contains no forbidden cumulative counters;
- journald remains bounded and operational;
- clean shutdown/state flush/serial release PASS;
- active step advances to DEV-012C.

### DEV-012C — offline bundle and installer

Scope:

- build the production AArch64 binary on the laptop with the Confirmed Bullseye
  ARM64 toolchain;
- verify AArch64 architecture and Bullseye GLIBC compatibility;
- verify dynamic runtime dependencies required by the tested WB8 image;
- prepare one local installation bundle containing:
  - production `dmxwb` binary;
  - static Web files;
  - `dmxwb.service`;
  - local installer;
  - default/example configuration;
  - any runtime files that are required beyond the tested stock WB image;
- record source SHA, artifact SHA256, architecture, GLIBC requirement and dynamic
  dependencies;
- install to the normal product locations:
  `/etc/dmxwb`, `/var/lib/dmxwb`, `/var/www/dmxwb`;
- preserve an existing valid user config/state on repeat installation;
- install/enable/start the service from local bundle contents.

The installer must not execute or require:

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

Production Art-Net identity must never use an invented OEM Code. If a registered
OEM Code is not yet available, development and acceptance may continue with
production ArtPollReply advertisement disabled until the identity is assigned.

PASS:

- bundle is created completely on the laptop;
- target dependency audit PASS for the tested WB8 software image;
- installer static audit PASS;
- repeat installation preserves existing valid config/state;
- no network-download path exists;
- no compiler/CMake/Node/Docker is required on WB8.

### DEV-012D — offline WB8 acceptance and gate closeout

Scope:

- disconnect external Internet while keeping the required local LAN;
- install DMXWB only from the prepared local bundle;
- record WB8 model/software version and source/binary/bundle identity;
- verify service start;
- perform a full WB8 reboot;
- verify automatic DMXWB startup after reboot;
- verify config/state restore;
- verify local Mosquitto and MQTT reconnect;
- verify local static Web after reboot;
- verify basic MQTT Fixture -> physical DMX operation;
- verify explicit `WB MQTT / ART-NET` Source selection;
- verify basic Art-Net -> physical DMX operation;
- verify deliberate process failure -> systemd recovery;
- verify Mosquitto restart -> in-process MQTT recovery without DMXWB process
  restart;
- verify standard WB HomeUI is not polluted by Fixture/Group/Scene devices;
- verify no external download or source compilation is needed on WB8;
- update `docs/PROJECT_STATE.md` and `README.md` with the actually accepted
  production installation and commands.

PASS:

- clean offline installation PASS;
- WB8 reboot and automatic service startup PASS;
- config/state restore PASS;
- Web/MQTT/basic physical DMX/basic Art-Net PASS;
- process failure is recovered by systemd;
- recoverable MQTT failure recovers in-process;
- no external Internet or source compilation is required on WB8;
- documentation matches the accepted installation;
- DEV-012 receives engineering PASS and current gate advances to DEV-013.

The full cross-subsystem acceptance and 24-hour run remain in DEV-013.

## Production items still Deferred

Until DEV-012 is Confirmed:

- fully offline bundle/installer acceptance;
- reboot acceptance;
- registered production Art-Net OEM identity if not yet assigned.

After DEV-012:

```text
DEV-013 — full integration and 24-hour final acceptance
```

## Build/test policy

```text
Windows host                 -> project files / ZIP / Git / WSL launch
Local Linux / WSL on laptop -> C++ build/tests + target build
Bullseye ARM64 toolchain     -> WB8 target artifact
WB8                          -> runtime/hardware/integration acceptance
Docker                       -> not used
```

Windows/MSVC is not part of the supported build/test matrix.
