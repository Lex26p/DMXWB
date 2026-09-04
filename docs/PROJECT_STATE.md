# PROJECT_STATE

**Last updated:** 2026-09-03

## Repository source base

```text
a3e8ab06b84f3c63f41422a5e0524fac32f15a6a
Current user-owned repository HEAD during DEV-012B3 acceptance
```

DEV-012B3 was accepted from a modified worktree based on this HEAD. The accepted
production artifact and all local changes belong to the user's next commit.

## Last confirmed engineering PASS

```text
DEV-013 — full integration and final acceptance
```

DEV-012C1 through DEV-012C4 are **Confirmed** by the user-reported PASS of the
focused reproducible bundle, install/update, remove/purge and final-bundle host
regressions. DEV-012D1 through DEV-012D3 are **Confirmed** by real-WB8 acceptance.
DEV-012E is **Confirmed** by the user's review of the corrected end-user guides.
DEV-013A is **Confirmed** by the user-reported final host + ARM64 acceptance PASS.
DEV-013B is **Confirmed** by real-WB8 acceptance of Fixture, Group, Scene, MQTT
and config/state restoration at Start Address 21 using the final DEV-013A archive.
DEV-013C is **Confirmed** by real-WB8 acceptance of short/long ArtDmx, non-default
mapping, explicit Source isolation, Hold Last/restarted Sequence recovery and
MQTT/Web recovery using the same final archive.
DEV-013D is **Confirmed** by final offline installation and full WB8 reboot
acceptance. DEV-013A through DEV-013D and the complete DEV-013 gate are Confirmed.

## Current engineering gate

```text
DEV-014 — corrective entity-management completion
```

DEV-012 and DEV-013 remain technically Confirmed. Post-acceptance user evaluation
found that the structural Web workflow does not match the required product flow:
Fixture creation is represented by a numeric count, arbitrary Fixture deletion is
not available, and Fixture/Group/Scene are intentionally hidden from standard WB
HomeUI. DEV-014 corrects these user-facing requirements before a new final package.

The current process checkpoint is:

```text
DEV-014C — clean-state acceptance and replacement final package
```

DEV-014A is **Confirmed** by the user-reported focused static PASS: dedicated Web
now provides explicit ordered Fixture Add/Delete before Group creation, removes
deleted Fixture membership from every Group and no longer exposes numeric Fixture
Count editing.

DEV-014B is **Confirmed** by the user-reported focused host PASS: Fixture, Group
and Scene MQTT controls now publish visible WB metadata, while their structural
creation and deletion remain exclusive to dedicated Web.

DEV-012B1 -> DEV-012B2 -> DEV-012B3 is complete. The independent Ultra review is
also complete and reported 11 concrete defect paths, resolved by DEV-012B4. A
second Ultra review identified 8 additional paths, all resolved and accepted in
DEV-012B5. DEV-012B5.1 through DEV-012B5.8 are Confirmed; the next implementation
checkpoint is DEV-012C.

DEV-012B1 completed the normative diagnostics correction without changing C++:

- production `/dmxwb/status` is factual and state-oriented;
- cumulative lifetime frame, packet, command, publication, snapshot, source-switch,
  failure and recovery totals are engineering/test instrumentation rather than
  production contract;
- current state, current configuration/Source, last error and recovery state remain
  operational diagnostics;
- snapshot generations/revisions, Art-Net sequence/source/sync state, config
  revision and monotonic Fixture/Group/Scene ID generators remain required
  algorithmic state.

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

### DEV-012B1 — production diagnostics contract correction — Completed

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

### DEV-012B2 — production / engineering instrumentation separation — Completed

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

#### DEV-012B2 execution substeps

The following substeps are an implementation plan inside DEV-012B2. They do not
change the normative scope, order or PASS criteria in `ROADMAP.md`.

1. **DEV-012B2.1 — instrumentation mode and ownership seam — Completed**
   - Scope: introduce one explicit `production / engineering` instrumentation
     mode and propagate it through the existing integrated runtime and subsystem
     constructors without duplicating algorithms; production selects `production`,
     while diagnostics and acceptance frontends select `engineering`.
   - PASS: all affected targets compile; existing engineering diagnostics remain
     enabled by default in directly tested components; production runtime owns one
     explicit disabled mode which is also preserved across runtime reconfiguration.
   - Evidence: user-reported native Linux warnings-as-errors build PASS.
2. **DEV-012B2.2 — DMX/router/coordinator hot-path isolation — Completed**
   - Scope: condition test-only accumulation in `DmxOutput`, `DmxSourceRouter` and
     `ArtNetSourceCoordinator`; retain factual state and algorithmic generations;
     remove production atomic counter traffic from the 44 Hz physical loop.
   - PASS: engineering tests retain their existing counter observations; production
     mode keeps those counters at zero while whole-snapshot routing, Hold Last,
     source switching and fixed 44 Hz output behavior remain unchanged.
   - Evidence: user-reported native Linux warnings-as-errors build and targeted
     DMX/router/Art-Net coordinator behavioral tests PASS.
3. **DEV-012B2.3 — MQTT/Art-Net instrumentation isolation — Completed**
   - Scope: condition test-only accumulation in `MqttClient`,
     `MqttRuntimeCoordinator` and `ArtNetRuntime`; retain connection, transport,
     source, sync, error and protocol/algorithm state required for operation.
   - PASS: engineering tests and acceptance frontends retain counters; production
     mode does not accumulate packet, command, publication, snapshot or recovery
     totals and continues normal reconnect/rebind behavior.
   - Evidence: user-reported native Linux warnings-as-errors build and targeted
     MQTT/Art-Net behavioral tests PASS.
4. **DEV-012B2.4 — factual production status and transition logging — Completed**
   - Scope: replace cumulative fields in `/dmxwb/status` with current factual
     state; derive journald error/recovery events from state transitions rather than
     counter deltas; keep lifecycle/configuration logging bounded.
   - PASS: status contains only allowed factual fields and algorithmic revision/ID
     state; MQTT, Art-Net and DMX error/recovery events are emitted once per state
     transition without high-rate logging.
   - Evidence: user-reported warnings-as-errors production build plus targeted
     Art-Net and operational-status tests PASS.
5. **DEV-012B2.5 — corrected build/static/acceptance contract — Completed**
   - Scope: update checks and acceptance expectations for the corrected production
     contract and add a build-level assertion against forbidden cumulative status
     fields.
   - PASS: the complete DEV-012B2 host/build checks satisfy the gate PASS above and
     the active step advances to DEV-012B3 only after user-reported PASS.
   - Evidence: user-reported static contract, complete native Linux
     warnings-as-errors build/tests and Bullseye ARM64 architecture/GLIBC/dependency
     audit PASS.

### DEV-012B3 — WB8 regression after counter isolation — Confirmed

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
- active step advances to DEV-012B4.

Acceptance report:

```text
docs/DEV012B3_COUNTER_ISOLATION_REGRESSION_REPORT.txt
=== DMXWB DEV-012B3 COUNTER ISOLATION REGRESSION PASS ===
```

Confirmed acceptance identity:

```text
source HEAD:       a3e8ab06b84f3c63f41422a5e0524fac32f15a6a
source worktree:   modified
artifact SHA256:   b8fd6bf64695a9e99ac0cddd7cc4c2f8505144c05296cc7b694bed8dc98c6ff8
target:            WB8 / wb-2606 stable / Bullseye
```

Confirmed real WB8 behavior:

- MQTT RED -> physical RED;
- Source ART-NET + ArtDmx BLUE -> physical BLUE;
- Source ART-NET -> WB MQTT returned to RED without changing the process PID;
- clean systemd stop caused no unwanted restart and flushed state;
- explicit systemd restart and SIGKILL recovery created the expected new PIDs;
- Mosquitto restart recovered in-process with the same `dmxwb` PID and continuous
  physical RED output;
- retained production status remained factual and contained no forbidden
  cumulative telemetry fields;
- journald contained 29 bounded lifecycle/recovery/source events;
- final Power OFF, clean stop, state flush and RS-485 release PASS;
- original service/files/retained MQTT environment was restored.

The independent Ultra review after DEV-012B3 is complete. It confirmed 11 defect
paths that must be corrected before DEV-012C. This finding changed the normative
roadmap by inserting DEV-012B4; it did not broaden the product into an independent
SCADA or change the remaining DEV-012C/DEV-012D product scope.

### DEV-012B4 — independent audit remediation — Confirmed

Exact goal: eliminate all 11 confirmed defect paths before offline packaging while
preserving the Confirmed physical DMX, explicit Source, MQTT, Art-Net, Web and
systemd behavior.

Complexity assessment: **high**. The findings cross the physical-output concurrency
boundary, two-file persistence, canonical validation, operational status ownership
and browser/backend command completion. One implementation step would be too large
and would make failures difficult to isolate. The fixed implementation plan is:

1. **DEV-012B4.1 — physical-output concurrency and Art-Net retry — Confirmed**
   - Findings: physical sink publish/reconfigure race; failed Art-Net generation is
     marked handled before successful physical routing.
   - PASS focus: deterministic publish/reconfigure interleaving, latest-snapshot
     preservation, bounded same-generation retry and no duplicate after success.
2. **DEV-012B4.2 — coherent config/state persistence transaction — Confirmed**
   - Finding: durable config may advance before debounced state, so a crash can
     restart from a mismatched pair and silently reset runtime state.
   - PASS focus: fault-injected commit boundaries always recover an old or new
     coherent pair; successful response follows durable coherent commit.
3. **DEV-012B4.3 — bounded valid names and monotonic stable IDs — Confirmed**
   - Findings: unbounded names can create an unreadable next-start config; malformed
     UTF-8 reaches retained MQTT/Web; deleted stable IDs can be reused.
   - PASS focus: 256-byte valid UTF-8 boundary, total serialized-size guard and
     rejection of counter rollback/reused Fixture/Group/Scene IDs.
4. **DEV-012B4.4 — persistence failure backoff and recoverable health — Confirmed**
   - Findings: overdue failed state save retries on every tick; immutable startup
     status cannot clear after successful corrective persistence.
   - PASS focus: bounded retry cadence, non-blocking DMX runtime, eventual save and
     factual error -> recovery status transition without process restart.
5. **DEV-012B4.5 — single factual operational-status owner — Confirmed**
   - Findings: Controller temporarily replaces subsystem status; Art-Net reports
     `live` before physical serial output is actually operational.
   - PASS focus: one `/dmxwb/status` owner and no false running/live state through
     commands, serial failure or physical recovery.
6. **DEV-012B4.6 — bounded Web command completion — Confirmed**
   - Finding: with broker connected and daemon absent, Config/Scene pending can be
     permanent; failed Scene Apply has no correlated backend result.
   - PASS focus: correlated results and bounded timeout clear only the matching
     pending command; reconnect never replays QoS 0 actions.
7. **DEV-012B4.7 — focused corrective regression on host and WB8 — Confirmed**
   - Scope: functional regression of only the changed subsystems, production ARM64
     build checks and focused WB8 runtime checks.
   - PASS focus: full host suite plus targeted WB8 concurrency, persistence restore,
     factual serial recovery and basic MQTT/Art-Net/Source behavior.

The detailed scope and functional PASS criteria for every substep are normative in
`docs/ROADMAP.md`. Before implementing each substep, its exact goal and current
complexity are stated again; implementation does not advance until the user reports
the requested functional checks as PASS.

#### DEV-012B4.1 acceptance result — Confirmed

- `DmxOutputPhysicalSink` is an explicit testable component with one mutex guarding
  backend lifetime, latest snapshot, port reconfiguration, status reads and stop;
- integrated runtime no longer returns an unguarded reference to the replaceable
  `DmxOutput`;
- an in-flight publication completes before the old output is stopped, and the
  replacement is preloaded with that latest whole snapshot;
- a failed Art-Net route keeps its generation pending and retries after a bounded
  50 ms interval; success completes the generation and prevents duplicates;
- deterministic functional tests cover the publish/reconfigure interleaving and
  bounded same-generation retry.

User-reported evidence: production `dmxwb` and the requested unit,
`dmx_output_physical_sink`, `dmx_source_router` and `artnet_source_coordinator`
targets built with warnings-as-errors; all four requested CTest cases PASS.

#### DEV-012B4.2 acceptance result — Confirmed

- the exact state corresponding to the next config revision is reconciled from the
  current in-memory state by stable Fixture ID and atomically staged first;
- atomic config rename is the only commit point: failure before it keeps the old
  config/model, while success selects a revision whose exact state is already
  durable;
- the prepared state is then atomically promoted to `state.json`;
- startup detects only a pending state matching the committed config revision and
  completes interrupted materialization before using ordinary state;
- compatibility recovery reconciles an older valid state by stable ID, preserving
  matching Fixtures and safely defaulting only newly configured Fixtures;
- pre-commit write failure, crash-boundary recovery, reorder/add preservation,
  finalize failure and later storage recovery have focused functional tests.

User-reported evidence: production `dmxwb` and the requested persistence storage,
persistence runtime, Group/Scene and MQTT Controller targets built successfully;
all requested CTest cases PASS.

#### DEV-012B4.3 acceptance result — Confirmed

- one shared UTF-8 validator enforces valid Fixture/Group/Scene names at the
  canonical config boundary and MQTT rename/Scene Create boundaries;
- valid names are bounded to 256 bytes; malformed UTF-8 and 257-byte names are
  rejected;
- the exact canonical serialized config, including its committed revision, is
  checked against `kPersistenceMaxFileBytes` before staging transaction state and
  again at the durable config writer boundary;
- config transitions reject rollback of Fixture/Group/Scene ID counters and reject
  introduction of any absent entity ID below the already allocated range;
- historical deleted Fixture IDs remain valid inside old Scene snapshots but cannot
  be assigned to a new Fixture;
- focused tests cover name boundaries, malformed UTF-8, oversized canonical config,
  all three counter types and all three stable-ID types.

User-reported evidence: production `dmxwb` and the requested persistence,
persistence storage/runtime, MQTT config, MQTT Group/Scene and Group/Scene targets
built successfully; all requested CTest cases PASS.

#### DEV-012B4.4 acceptance result — Confirmed

- failed scheduled state writes keep dirty state but set an explicit 2 s
  retry-not-before deadline, preventing a write attempt on every runtime tick;
- new state changes cannot bypass an active failure delay, while forced shutdown
  flush remains immediate and reports its actual result;
- pending config-state preparation and cleanup failures use the same bounded retry
  path rather than forming a separate tight loop;
- persistence health now tracks current config/state/restore errors and whether a
  startup fallback is actually active;
- successful corrective state save or coherent config transaction clears recovered
  errors without restarting the process;
- integrated `/dmxwb/status` distinguishes `configuration: fallback` from a current
  runtime persistence `error`, includes the factual error, and returns to
  `running/ok` after recovery;
- focused tests cover retry deadlines, storage recovery, live health recovery,
  startup fallback correction and operational status transitions.

User-reported evidence: production `dmxwb` and the requested persistence storage,
persistence runtime and operational-status targets built successfully; all
requested CTest cases PASS.

#### DEV-012B4.5 acceptance result — Confirmed

- `MqttController` no longer builds or publishes `/dmxwb/status` or the standard WB
  Status state during Source, Config, Scene or reconnect paths;
- Controller model republish helpers are structurally limited to config/state and
  Source state, so a command cannot erase a current subsystem error with synthetic
  `controller/running` values;
- integrated runtime remains the sole publisher of both retained operational
  `/dmxwb/status` and `/devices/dmxwb/controls/status`;
- Art-Net output mode requires selected ART-NET, ACTIVE source, accepted physical
  route and factual running/open physical DMX before reporting `live`;
- serial failure/unopened state reports `hold_last`, and physical recovery changes
  it to `live` only when the output is operational;
- focused Controller, MQTT reconnect, contract and operational-status tests enforce
  the single-owner and physical-readiness conditions; historical static contracts
  were updated to the same ownership rule.

User-reported evidence: the requested Linux build, targeted CTest cases and static
operational contract all PASS.

#### DEV-012B4.6 acceptance result — Confirmed

- Web Config Set and every Web Scene lifecycle operation now use a unique
  `request_id` and accept only the matching non-retained backend result;
- Scene Rename and Apply have dedicated correlated `/dmxwb/scenes/<id>/rename` and
  `/dmxwb/scenes/<id>/apply` command paths, while standard WB Scene controls remain
  compatible;
- applying a Scene deleted by another client returns correlated `not_found` instead
  of leaving the browser pending;
- Config and Scene pending state has a bounded 5 s deadline and is also cleared by
  matching factual state or MQTT disconnect;
- reconnect only restores subscriptions and retained factual state; QoS 0 commands
  are not queued or replayed;
- focused MQTT Group/Scene and Web static contracts cover request matching,
  deleted-Scene Apply and bounded completion.

User-reported evidence: production `dmxwb` and the focused MQTT Group/Scene target
built successfully; the requested CTest and Web config/scene static contracts PASS.

#### DEV-012B4.7 acceptance result — Confirmed

- one host runner configures warnings-as-errors, builds all native Linux targets,
  runs the complete CTest suite and both affected Web functional contract chains;
- the same runner creates and verifies the Bullseye ARM64 production artifact,
  including AArch64, GLIBC and runtime dependency checks;
- a dedicated WB8 wrapper reuses the proven environment backup/restore and cleanup
  framework but does not repeat the full DEV-012B3 acceptance;
- the focused WB8 path checks basic MQTT/Art-Net/explicit Source behavior, Art-Net
  traffic during runtime DMX Port reconfiguration, coherent config/state recovery
  after service restart and factual serial failure/recovery in the same process;
- the alternative serial device node is moved only for the bounded recovery check
  and is restored by the cleanup trap on PASS or FAIL;
- no Git operation is performed by the new DEV-012B4.7 runners.

User-reported evidence: native warnings-as-errors build, full CTest, affected Web
contracts and Bullseye ARM64 checks PASS. The focused WB8 regression also confirmed
Art-Net traffic during port reconfiguration, coherent config/state restart,
factual serial reconnect/recovery in one process and basic MQTT/Art-Net/Source
behavior. The acceptance environment was restored.

The second independent Ultra review after this PASS identified 8 additional concrete
defect paths. They do not invalidate the observed B4.7 behavior, but they block
offline packaging and are now the fixed DEV-012B5 corrective gate.

### DEV-012B5 — post-regression independent audit remediation — Confirmed

Exact goal: eliminate all 8 second-review defect paths without changing DMXWB into
an independent SCADA and without reopening already Confirmed unrelated scope.

Complexity assessment: **high**. The work is split into eight fixed substeps:

1. **DEV-012B5.1 — Art-Net accepted-traffic liveness and sequence recovery — Confirmed**
   - rejected Sequence traffic must not postpone LOST; a restarted controller must
     establish a new baseline after the normative timeout.
2. **DEV-012B5.2 — persistence fallback and file-identity safety — Confirmed**
   - corrupt config must not rewrite valid state; aliased config/state paths must be
     rejected before any write.
3. **DEV-012B5.3 — factual daemon availability gate for Web commands — Confirmed**
   - broker connectivity alone must not enable commands while the daemon is off.
4. **DEV-012B5.4 — durable retained MQTT cleanup — Confirmed**
   - removed entity tombstones must survive disconnect and process restart.
5. **DEV-012B5.5 — Config uncertain-outcome reconciliation — Confirmed**
   - lost results must reconcile against factual config instead of guaranteeing a
     revision-conflicting retry.
6. **DEV-012B5.6 — idempotent Scene Create retry — Confirmed**
   - retrying the same request ID must return the same Scene and never duplicate it.
7. **DEV-012B5.7 — strict Web numeric validation — Confirmed**
   - invalid visible values must block Apply rather than silently sending old data.
8. **DEV-012B5.8 — focused corrective regression on host and WB8 — Confirmed**
   - full host/ARM64 verification plus only the affected high-risk WB8 paths.

The normative scope and PASS criteria are fixed in `docs/ROADMAP.md`.

#### DEV-012B5.1 acceptance result — Confirmed

- Art-Net activity timestamp, ACTIVE transition and conflict clearing now occur
  only after Sequence validation accepts the ArtDmx packet;
- stale/duplicate traffic cannot postpone the three-second LOST deadline or clear
  an existing source conflict;
- LOST continues to hold the last committed whole snapshot while resetting source,
  Sequence and sync tracking;
- the first valid low Sequence after LOST establishes a new baseline for a restarted
  controller;
- a focused core regression covers high-to-low restart traffic, repeated stale
  packets near the deadline, conflict preservation, LOST timing and recovery.

User-reported evidence: production `dmxwb` and the focused Art-Net core target built
successfully; the targeted Art-Net core CTest PASS.

#### DEV-012B5.2 acceptance result — Confirmed

- config load failure now returns immediately with a safe in-memory fallback and
  explicitly disables state writes instead of reconciling valid state against zero
  Fixtures;
- source/Fixture changes, scheduled saves and forced shutdown flush cannot dirty or
  overwrite state while config fallback is active;
- restoring a valid config on restart loads the untouched state; explicit config
  repair without restart reconciles from the existing valid state before its
  coherent transaction;
- one reusable path-identity boundary rejects literal, normalized, symlink and
  hard-link config/state aliases;
- production CLI validates paths before signals/runtime construction, and
  `PersistenceRuntime` independently enforces the same invariant before loading;
- focused tests cover byte-for-byte state preservation through tick/flush, value
  recovery, safe explicit repair and alias rejection without file mutation.

User-reported evidence: the targeted persistence build and regression PASS.

#### DEV-012B5.3 acceptance result — Confirmed

- browser-to-broker connectivity is no longer sufficient to publish commands;
- Web now subscribes to retained/LWT `/devices/dmxwb/controls/status` and tracks
  factual daemon availability separately from MQTT WebSocket connectivity;
- commands remain disabled while the daemon status is unknown or `off`, including
  the interval between browser reconnect and delivery of retained status;
- both `running` and `error` identify a live daemon, so recoverable subsystem errors
  do not block commands needed for recovery;
- transition to unavailable state clears live, Config and Scene pending operations
  as unconfirmed and changes the connection indicator from online;
- browser disconnect clears the cached daemon status, preventing a stale online
  decision after reconnect;
- static asset versions were advanced so an installed browser cannot keep the old
  command-gating modules in cache;
- one focused Web contract check covers the status topic, subscription, availability
  mapping, dual connection gate and pending cleanup.

User-reported evidence: the focused Web daemon availability contract PASS.

#### DEV-012B5.4 acceptance result — Confirmed

- `state.json` now carries backward-compatible pending retained-cleanup stable IDs
  for Fixture, Group and Scene;
- config transactions add removed IDs to the coherent state before the atomic
  config commit, so a crash cannot lose knowledge of deleted entities;
- active/unallocated/duplicate cleanup IDs are rejected, while existing monotonic
  ID rules prevent a pending deleted ID from being reused;
- Controller builds tombstones only from the durable intent and keeps them separate
  from ordinary config/state republish batches;
- the Mosquitto transport tracks every QoS 1 cleanup message ID and reports success
  only after all corresponding PUBACK callbacks;
- disconnect or partial submission marks the in-flight batch failed; orchestration
  retains the intent and retries the complete idempotent batch after recovery;
- a successful batch removes exactly its delivered ID snapshot; removals added while
  it was in flight remain pending for a subsequent batch;
- restart restores unacknowledged cleanup, while a crash after PUBACK but before the
  next state save can only cause a harmless tombstone repetition;
- focused runtime coverage exercises Fixture/Group/Scene removal, unacknowledged
  restart, reconnect retry, delivery failure, PUBACK completion, active-ID safety
  and persistence of the acknowledged result.

User-reported evidence: the production target and affected persistence/MQTT test
targets built successfully; all four targeted CTests Passed.

#### DEV-012B5.5 acceptance result — Confirmed

- timeout, daemon unavailability and browser disconnect preserve the Config request
  ID, complete proposal and base revision as an uncertain transaction;
- while reconciliation is pending, structural editing and another Apply remain
  blocked, and Web explicitly refreshes the retained config when connected;
- a factual config matching the proposal except server revision completes the
  original operation and clears the dirty draft without replay;
- a post-request factual config with unchanged revision releases the original draft
  for explicit retry using its still-valid base revision;
- a differing factual config with another revision retains the local draft as an
  explicit stale/conflict state and blocks publication of its obsolete revision;
- late correlated success/error results are handled against the same saved request;
  `revision_conflict` waits for factual retained config before allowing action;
- the only Config Set publication remains the explicit Apply path;
- static asset versions were advanced and one focused Web contract checks all three
  factual reconciliation outcomes, refresh, stale blocking and no auto replay.

User-reported evidence: the focused Web Config uncertain-outcome reconciliation
contract PASS.

#### DEV-012B5.6 acceptance result — Confirmed

- `state.json` now contains a backward-compatible bounded history of the 64 latest
  successful Scene Create request IDs, Names, stable Scene IDs and commit revisions;
- the idempotency record is prepared in the same coherent state transaction before
  the atomic config commit, eliminating a crash window between Scene creation and
  durable deduplication;
- the same request ID and Name returns the original Scene ID, original revision and
  identical correlated result without another config commit or ID allocation;
- reuse of a retained request ID with another Name returns `idempotency_conflict`
  without model mutation;
- old records are evicted in insertion order only after the fixed capacity is
  exceeded, while monotonic Scene IDs remain unchanged;
- Web preserves an uncertain Create request and its payload across timeout,
  disconnect and daemon unavailability, disables Name editing and uses the same
  request ID for an explicit retry;
- late or retried correlated results clear the same uncertain request; Web no longer
  promises that a newly generated Scene lifecycle request is a safe retry;
- focused tests cover identical replay, payload conflict, restart durability,
  original correlated outcome, monotonic allocation, bounded eviction and legacy
  state compatibility; a focused Web contract covers same-request retry behavior.

User-reported evidence: the targeted native persistence/MQTT and focused Web Scene
Create idempotency regression PASS.

#### DEV-012B5.7 acceptance result — Confirmed

- the three numeric structural fields now use text-preserving numeric inputs, so an
  empty, non-numeric or out-of-range value remains visible instead of being silently
  replaced by the previous draft value;
- one validation path applies explicit ranges `0..75`, `1..300` and `0..32767` on
  every input and again immediately before Config Set publication;
- Fixture Count and Start Address additionally enforce the physical 300-channel
  boundary as a combined constraint;
- invalid fields receive factual `aria-invalid` and visible error treatment, show a
  concrete Russian error, disable Apply and never mutate the draft with stale data;
- Reset remains available for invalid raw input even if no valid draft mutation was
  made, and restores the factual draft values;
- corrected valid values update the draft, while pre-publication equality checks
  ensure the proposal contains exactly the numeric values currently shown;
- a focused Web contract covers visible invalid input, boundaries, combined physical
  range, Apply/Reset gating and validation before MQTT publication.

User-reported evidence: the focused Web strict numeric validation contract PASS.

#### DEV-012B5.8 acceptance result — Confirmed

- one host runner reuses the established Bullseye ARM64 workflow exactly once; that
  workflow performs the native warnings-as-errors build, full CTest, production
  cross-build and AArch64/GLIBC/runtime-dependency checks;
- the host runner then executes only the four Web contracts affected by B5.3,
  B5.5, B5.6 and B5.7;
- the WB8 runner temporarily installs the current production binary and static Web,
  preserving and restoring the previous service files, config/state, Web tree and
  every retained topic touched by the regression on PASS or failure;
- an offline-broker start plus process crash proves pending Fixture/Group/Scene
  tombstones remain durable and are cleared only after reconnect and delivery;
- identical Scene Create request ID and payload are replayed after service restart
  with the original Scene ID/revision and without a second config mutation;
- Sequence 128 followed by a restarted controller repeatedly sending Sequence 1
  proves that rejected traffic cannot retain the blue frame past the 3-second LOST
  boundary and that the green frame establishes a new baseline;
- corrupt-config fallback is held for more than the state debounce interval and
  verified not to change the valid `state.json` SHA256 before normal recovery;
- current Web behavior is checked against real retained/LWT `running -> off ->
  running` transitions for clean stop/start and crash recovery without page reload;
- the runner does not repeat the broad DEV-012B3/B4 lifecycle, serial-recovery,
  HomeUI or journald acceptance.

The first focused WB8 run stopped at its intended offline-broker startup case: the
production process treated an unavailable initial MQTT connection as fatal and
entered a systemd restart loop. The transport now owns a bounded-backoff network
worker: successful local initialization starts the daemon independently of current
broker availability, while connection establishment and recovery remain in-process
and never run on the DMX output context. A focused MQTT client regression covers
startup and clean stop with an unavailable local endpoint.

The next focused run confirmed pending retained cleanup, Scene Create idempotency,
Art-Net restart recovery, corrupt-config state preservation and the manual Web
`running -> off -> running` behavior. Its final automatic LWT trace exposed an
acceptance race: periodic factual `running` republishes exhausted a fixed three-message
capture before post-crash recovery. The trace now ignores duplicate states, waits for
the actual `running -> off -> running` transition and independently requires a changed
systemd MainPID.

A subsequent run again confirmed the offline-start, retained-cleanup and Scene paths,
but exposed another probe race in the already previously passing Art-Net case: status
polling started only after the fixed-Sequence sender ended and could miss the bounded
`ACTIVE / last_sequence=1` window. The sender now runs concurrently with that factual
status check; rejected duplicate packets still do not extend accepted-source liveness.

User-reported evidence: native warnings-as-errors/full CTest, Bullseye ARM64 build
and focused real-WB8 regression PASS. The run confirmed offline-broker startup,
durable retained cleanup, Scene Create idempotency, Art-Net Sequence restart recovery,
corrupt-config state preservation and Web command availability through clean
stop/start and crash recovery. Active work advances to DEV-012C.

### DEV-012C — offline bundle and installer

Exact goal: produce one self-contained WB8 installation bundle from the already
Confirmed application artifact, with safe offline install/update/removal behavior
and no source compilation or external downloads on the controller.

Complexity assessment: **high**. The current project already has the production
systemd unit, static Web and a verified Bullseye ARM64 build workflow, but it has no
bundle builder, default/example config, installer, uninstaller or bundle-integrity
contract. DEV-012C is therefore split into four fixed implementation substeps:

1. **DEV-012C1 — bundle layout, metadata and reproducible builder — Confirmed**
   - Scope: define the root-relative offline bundle layout; add the canonical
     default/example config; package only the production binary, static Web,
     systemd unit and deployment scripts; generate artifact/source identity,
     architecture/GLIBC/dependency metadata and SHA256 checksums without using the
     network.
   - PASS: a focused host contract verifies exact required/forbidden bundle paths,
     checksum coverage, metadata fields, executable/file modes and absence of
     source/build-tree/test artifacts or online commands.
   - Implementation checkpoint: a DEV-012C1 payload builder now stages target-root
     paths under one archive root, records caller-supplied source identity,
     application/artifact/ARM64/GLIBC/dependency metadata, covers every payload file
     with SHA256, normalizes ownership/timestamps/modes and creates a deterministic
     gzip archive. The payload includes the production daemon, systemd unit, exact
     static Web and canonical zero-Fixture example config. Installer/uninstaller are
     intentionally not represented as complete until C2/C3 and enter the final
     installation bundle only in C4.
2. **DEV-012C2 — safe fresh install and in-place update — Confirmed**
   - Scope: implement one idempotent offline installer with preflight checks,
     checksum verification, required stock-WB8 dependency checks, correct
     permissions and atomic replacement of managed binary/Web/unit files; first
     install creates config/state locations, while every update preserves existing
     valid user config/state and leaves no partial active installation on failure.
   - PASS: focused isolated-root install/update regression proves initial layout,
     repeatability, managed-file replacement, config/state byte preservation,
     checksum/dependency rejection and absence of network/build operations.
   - Implementation checkpoint: the offline installer now verifies exact required
     checksum coverage, the complete SHA256 file, manifest target/architecture and
     payload artifact identity before mutation. Production mode requires root,
     AArch64, Debian 11, resolved dynamic dependencies and systemd; conventional
     non-root `DESTDIR` staging skips all service actions for isolated verification.
     Managed binary/unit/Web files use same-filesystem atomic replacement with
     rollback, first install creates the canonical config only when absent, and
     update never writes existing config/state. A focused regression covers fresh
     install, managed-file update, byte preservation, checksum rejection, injected
     rollback and idempotent repeat update.
3. **DEV-012C3 — safe removal and explicit purge — Confirmed**
   - Scope: add a bounded removal path that stops/disables the service and removes
     only DMXWB-managed binary/Web/unit files; normal removal preserves config/state,
     while irreversible data purge requires a separate explicit option and never
     follows from ordinary uninstall/update.
   - PASS: focused isolated-root regression proves uninstall idempotency, exact
     managed-file removal, preservation of user data by default, explicit purge and
     refusal of ambiguous/destructive targets.
   - Implementation checkpoint: the bounded uninstaller removes only the production
     binary, systemd unit and five known static Web files in normal mode. Existing
     `config.json` and `state.json` remain byte-for-byte untouched unless the caller
     explicitly supplies `--purge`; even purge removes only those two known data
     files and preserves unrelated files in the same directories. The script rejects
     root `DESTDIR`, symbolic-link targets and symbolic-link path components before
     mutation, restores removed files after a failed operation, is idempotent and
     performs no systemd action in isolated `DESTDIR` verification mode.
4. **DEV-012C4 — final host bundle regression — Confirmed**
   - Scope: rebuild the production Bullseye ARM64 artifact once, create the final
     local bundle, verify archive layout/checksums/identity/dependencies and run only
     the install/update/remove regressions introduced by C1-C3.
   - PASS: reproducible bundle creation and all DEV-012C host checks PASS; no
     compiler/CMake/Node/Docker/network operation is needed on WB8; active work
     advances to DEV-012D for real offline installation/reboot acceptance.
   - Implementation checkpoint: the final deterministic archive now combines the
     accepted payload, `install.sh` and `uninstall.sh` under one root and identifies
     itself as `production-offline`. A focused production-only mode of the existing
     Bullseye cross-builder rebuilds just `dmxwb`; the default full build behavior is
     unchanged. The C4 regression verifies exact current content, modes, ownership,
     timestamps, SHA256 coverage, AArch64/Bullseye metadata and absence of target
     compilation/online operations, then applies the accepted C2/C3 behavior checks
     to the scripts and payload extracted from that final archive.

The order and scope of C1-C4 are fixed for this gate. New facts that invalidate
this plan require an explicit revision before further implementation.

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

### DEV-012D — offline WB8 acceptance

Exact goal: prove that the accepted final archive installs and operates on the real
WB8 using only the controller's stock services and local LAN, survives reboot and
recoverable failures, and supports the accepted update/removal lifecycle without
losing user data.

Complexity assessment: **high**. Installation, update/removal, controller reboot,
physical DMX and service recovery have different failure/cleanup boundaries. The
fixed execution plan is:

1. **DEV-012D1 — clean offline installation and basic operation — Confirmed**
   - Scope: record target/bundle identity, back up the existing DMXWB environment,
     perform a clean install from the local final archive while external Internet is
     unavailable, verify installer-created layout, systemd enable/start, local MQTT,
     local-LAN Web and one MQTT Fixture driving physical DMX, then restore the exact
     original environment.
   - PASS: the bundled installer succeeds without download/build operations; installed
     hashes/config/modes and running service are correct; Web and MQTT are available;
     the physical Fixture responds; the original WB8 files, service state and touched
     retained topics are restored.
   - Implementation checkpoint: one guarded acceptance script now confirms the
     external-Internet disconnect, records final bundle/artifact/source and WB8
     identity, backs up exact existing DMXWB files/directories, service enable/active
     state and every retained topic touched by the test, and then creates a genuinely
     clean target layout. It transfers only the final archive, invokes its own
     `install.sh`, checks installed hashes/modes/default config, systemd, local MQTT,
     nginx Web locally and through the user's LAN browser, then installs a one-Fixture
     acceptance config and verifies MQTT-driven physical red output. Every normal or
     failed exit powers off the test Fixture where possible and restores the original
     files, service state and retained topics.
   - User-reported real-WB8 evidence: final archive SHA256, WB8 rev. 8.5.1 / T507,
     wb-2606 stable Bullseye and kernel 6.8.0-wb160 were recorded; clean offline
     install, systemd enable/start, local MQTT/nginx Web, LAN browser, physical red,
     clean stop/port release and exact environment restoration all reported PASS in
     `docs/DEV012D1_OFFLINE_INSTALL_REPORT.txt`.
2. **DEV-012D2 — update, removal and reinstall lifecycle — Confirmed**
   - Scope: on real WB8 test in-place update, normal removal, reinstall and explicit
     purge against isolated acceptance data, including preservation of config/state
     across update/remove/reinstall and preservation of unrelated files.
   - PASS: accepted lifecycle commands are idempotent, normal update/removal preserve
     user data byte-for-byte, explicit purge removes only known acceptance data, and
     the original controller environment is restored.
   - Implementation checkpoint: a guarded real-WB8 lifecycle scenario now creates a
     valid one-Fixture config/state baseline and three unrelated sentinel files. It
     updates an active installation after deliberately changing one managed Web file,
     verifies exact managed replacement plus byte-preserved config/state, performs
     normal removal twice, reinstalls from the preserved data and confirms physical
     state restoration, then runs explicit purge twice. Purge must remove only the two
     known DMXWB data files while every unrelated sentinel survives. The same bounded
     backup/retained-topic cleanup used by D1 restores the original controller state
     on success or failure.
   - User-reported real-WB8 evidence: active update replaced a deliberately changed
     managed Web file without changing config/state; normal removal twice preserved
     those files; reinstall restored the physical red state; explicit purge twice
     removed only known data while unrelated sentinels survived; final environment
     restoration PASS. Evidence is recorded in
     `docs/DEV012D2_LIFECYCLE_REPORT.txt`.
3. **DEV-012D3 — reboot and runtime recovery acceptance — Confirmed**
   - Scope: install acceptance configuration, reboot the WB8, verify automatic startup,
     config/state/Web/MQTT restoration, basic MQTT and Art-Net physical paths, explicit
     Source switching, process-crash recovery, Mosquitto in-process recovery and clean
     standard WB HomeUI, then restore the original environment.
   - PASS: all normative DEV-012D reboot/runtime criteria pass and the controller is
     returned to its original state; active work advances to DEV-012E.
   - Implementation checkpoint: a guarded final acceptance script stores the complete
     original DMXWB environment and touched retained topics under `/var/tmp`, installs
     the accepted archive and persists a red one-Fixture state, then records boot ID
     and performs one real WB8 reboot. It waits for the SSH port to go down and return,
     opens a new authenticated connection and requires a changed boot ID before testing
     systemd autostart, config/state, MQTT, Web, physical MQTT/Art-Net output, explicit
     Source switching, SIGKILL recovery, in-process Mosquitto recovery and standard WB
     HomeUI. Cleanup can reconnect after an interrupted reboot and restores the saved
     environment before removing the persistent acceptance directory.
   - Confirmed evidence: real WB8 changed boot ID, restored config/state and physical
     red output after reboot, served Web, switched MQTT -> Art-Net blue -> MQTT red,
     recovered from SIGKILL with a new PID, recovered Mosquitto without changing PID,
     kept standard WB HomeUI clean, released serial and restored the original
     environment. Evidence is recorded in
     `docs/DEV012D3_REBOOT_RUNTIME_REPORT.txt`.

The order and scope of D1-D3 are fixed. A new fact that invalidates this plan requires
an explicit revision before further implementation.

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

PASS:

- clean offline installation PASS;
- WB8 reboot and automatic service startup PASS;
- config/state restore PASS;
- Web/MQTT/basic physical DMX/basic Art-Net PASS;
- process failure is recovered by systemd;
- recoverable MQTT failure recovers in-process;
- no external Internet or source compilation is required on WB8;
- active step advances to DEV-012E.

### DEV-012E — Web and installation/maintenance instructions — Confirmed

Scope:

- create a step-by-step Web UI guide covering connection state, Source, Fixture,
  Group, Scene and configuration workflows;
- create a step-by-step offline installation, update and removal guide;
- give verification commands separately from installation/update/removal commands;
- distinguish normal removal that preserves user config/state from explicit purge;
- end the operations guide with a complete list of supported commands and a short
  description of each command;
- link both guides from `README.md`.

PASS:

- instructions describe only behavior and commands accepted in DEV-012C/DEV-012D;
- all Web functions and all install/update/remove procedures are covered;
- verification commands and expected results are explicit;
- the final command reference is complete;
- documentation matches the accepted application;
- DEV-012 receives engineering PASS and current gate advances to DEV-013.

Implementation checkpoint:

- `docs/WEB_USER_GUIDE.md` describes connection/daemon availability, factual status,
  explicit Source selection, all Fixture/Group/Scene controls, structural settings,
  numeric validation, revision conflicts and automatic reconnect behavior;
- `docs/INSTALL_UPDATE_REMOVE_GUIDE.md` gives separate tested procedures for fresh
  offline installation, update, normal removal, reinstall and explicit purge;
- mutation procedures and read-only verification commands are separated;
- normal removal is explicitly data-preserving and purge is explicitly irreversible;
- the final reference covers bundle, installer, production executable, systemd,
  journald and all supported MQTT command surfaces;
- `README.md` links both manuals.

No code or deployment behavior changed in DEV-012E. The user reviewed and accepted
the corrected end-user installation flow based only on the ready installation
package and fixed `/root/dmxwb-installer` directory. DEV-012 is complete.

The full cross-subsystem final acceptance is complete.

## DEV-013 execution plan

Exact goal: prove that the accepted DMX, Fixture/Group/Scene, MQTT, Art-Net, Web,
persistence, systemd and offline deployment paths work together as one final WB8
application. DEV-013 adds no new product feature; a failure is corrected only in
the subsystem and substep where it is observed.

Complexity assessment: **very high**. Host automation, physical lighting behavior,
network recovery and destructive installation/reboot boundaries must remain
separate so a failure has one clear cause. The user-approved execution plan is:

1. **DEV-013A — final host, automated test and ARM64 bundle acceptance — Confirmed**
   - Scope: clean native Linux warnings-as-errors build, every registered CTest,
     the current transitive Web functional contracts, Bullseye ARM64 production
     build and reproducible final offline-bundle validation.
   - PASS: all automated checks pass; the resulting archive contains the exact
     tested production binary and becomes the only artifact used by B-D.
   - Implementation checkpoint: one wrapper reuses the existing CTest, Web and
     DEV-012C4 mechanisms without separately repeating historical Web prerequisite
     chains; it records `docs/DEV013A_FINAL_HOST_REPORT.txt`.
   - Confirmed evidence: the user reported `dev013a_result: PASS`; the accepted
     final archive is `artifacts/offline/dmxwb-0.1.0-wb8-bullseye-arm64.tar.gz`
     with SHA-256
     `f94fe7a4504310c524197854528d9aca2c43814d78657390f702f40a26a688bd`
     and `source_id=dev013a-final`.
2. **DEV-013B — integrated Fixture/Group/Scene/MQTT/persistence acceptance — Confirmed**
   - Scope: on real WB8 verify the final artifact's physical RGBW Fixture behavior,
     Group operations, Scene lifecycle/atomic apply, MQTT state and persistence
     across service restart using a non-default Start Address.
   - PASS: the complete logical-lighting path reaches physical DMX correctly and
     all accepted logical state returns after restart.
   - Confirmed evidence: the user reported `dev013b_result: PASS` on real WB8 at
     Start Address 21; Fixture RGBW/Brightness/Power/Reset, two-member Group,
     Scene create/apply/overwrite/rename/delete, stable Scene ID, names and
     config/state restoration after service restart all passed. Evidence is in
     `docs/DEV013B_INTEGRATED_LOGICAL_REPORT.txt`.
3. **DEV-013C — integrated Art-Net/Source/Web/recovery acceptance — Confirmed**
   - Scope: verify physical Art-Net mapping, short/long input, explicit Source
     switching, Hold Last and bounded recovery after source/network interruption,
     Web reconnect/availability and in-process subsystem recovery.
   - PASS: no mixed frame or automatic Source change occurs, recovery needs no
     manual DMXWB restart, and Web returns to factual operation.
   - Implementation checkpoint: one real-WB8 scenario installs only the archive
     accepted in DEV-013A, uses Start Address 21 and verifies 512/short/512 ArtDmx,
     background logical state, explicit Source boundaries, LOST/Hold Last,
     restarted Sequence recovery, Mosquitto/Web reconnect and Web availability
     across service stop/start. It restores the original controller environment
     and records `docs/DEV013C_INTEGRATED_ARTNET_WEB_REPORT.txt`.
   - Confirmed evidence: the user reported `dev013c_result: PASS` on real WB8.
     Physical non-default Art-Net mapping, short-frame channel retention, long
     frame replacement, explicit Source boundaries, LOST/Hold Last, Sequence=1
     recovery in the same process, Mosquitto/Web reconnect and factual Web
     availability all passed.
4. **DEV-013D — final offline install/reboot and closeout — Confirmed**
   - Scope: install only the archive accepted in A on WB8 without external Internet,
     verify autostart and persisted integrated state after reboot, then record final
     platform/artifact identity and restore the original controller environment.
   - PASS: final installation and reboot acceptance pass, documentation/state match
     the tested release, and DEV-013 receives FINAL PASS.
   - Implementation checkpoint: the final wrapper reuses the already accepted
     persistent backup/reboot mechanism, requires the exact DEV-013A source ID and
     archive SHA-256, records WB8/toolchain/sysroot identity, verifies the installed
     binary hash, reboot/autostart/config/state/Web and one post-reboot physical
     MQTT/Art-Net Source cycle. Recovery checks already passed in DEV-013C are not
     repeated. It records `docs/DEV013D_FINAL_OFFLINE_REBOOT_REPORT.txt`.
   - Confirmed evidence: the user reported `dev013d_result: PASS`. The exact
     DEV-013A archive and binary hashes matched, offline installation completed,
     WB8 boot ID changed, systemd autostarted DMXWB, config/state and physical red
     output returned, Web opened from LAN, post-reboot Art-Net blue and explicit
     return to MQTT red passed, the serial port was released and the original
     controller environment was restored.

The user-owned 24-hour observation is intentionally outside this execution plan.
Requirements evidence is collected by A-D; there is no separate audit-only step.
All four planned substeps are Confirmed. DEV-013 has FINAL PASS.

## Final tested release configuration

```text
Application version:       0.1.0
Final source ID:           dev013a-final
Offline bundle:            artifacts/offline/dmxwb-0.1.0-wb8-bullseye-arm64.tar.gz
Offline bundle SHA-256:    f94fe7a4504310c524197854528d9aca2c43814d78657390f702f40a26a688bd
Production binary SHA-256: 6dbff5ea94399075392a427f9bbf089a76ed1474900b3d5f44ece3925f27391d
Target model:              Wiren Board rev. 8.5.1 (T507)
WB release:                wb-2606 stable, WB8/Bullseye
Kernel:                    6.8.0-wb160
Architecture:              AArch64
DMX port:                  /dev/ttyRS485-1
Final fixture Start Addr:  21
Art-Net Port-Address:      0
Target toolchain:          Bullseye GCC 10.2.1 aarch64-linux-gnu-g++
Target sysroot:            Debian 11 Bullseye ARM64, libmosquitto 2.0.11
```

Final evidence:

- `docs/DEV013A_FINAL_HOST_REPORT.txt` — host tests, Web contracts, ARM64 build and
  reproducible final archive;
- `docs/DEV013B_INTEGRATED_LOGICAL_REPORT.txt` — Fixture/Group/Scene/MQTT and
  service-restart persistence on physical DMX;
- `docs/DEV013C_INTEGRATED_ARTNET_WEB_REPORT.txt` — Art-Net/Source/Web and
  recoverable network/subsystem failures;
- `docs/DEV013D_FINAL_OFFLINE_REBOOT_REPORT.txt` — exact offline package,
  platform identity, full WB8 reboot/autostart and post-reboot operation.

## DEV-014 corrective execution plan

Exact goal: complete the user-facing entity lifecycle without changing the core
DMX/Art-Net architecture. A Fixture is created first, Groups only select existing
Fixtures, any selected Fixture can be removed, a clean configuration starts empty,
and created Fixture/Group/Scene devices are visible and controllable in standard WB
HomeUI.

Complexity assessment: **high**. The correction changes Web draft semantics, MQTT
metadata visibility, accepted requirements and the final package. The user-approved
execution order is:

1. **DEV-014A — explicit Fixture CRUD in dedicated DMXWB Web**
   - Scope: replace numeric Fixture Count editing with a dedicated Fixture list,
     explicit Add/Delete actions and stable IDs; deletion removes the Fixture from
     every Group while historical Scene snapshots remain historical.
   - PASS: from an empty draft the user can add Fixtures before Groups, delete any
     selected Fixture, see derived DMX addresses and apply the full config once.
   - Confirmation: user-reported focused static PASS.
2. **DEV-014B — standard WB HomeUI visibility**
   - Scope: publish created Fixture/Group/Scene metadata as visible WB devices;
     structural create/delete remains in `/dmxwb/`, while standard WB HomeUI exposes
     live controls for already created entities.
   - PASS: standard WB HomeUI shows and controls created Fixture/Group/Scene without
     changing explicit Source behavior.
   - Confirmation: user-reported focused host PASS.
3. **DEV-014C — clean-state acceptance and replacement final package**
   - Scope: begin with zero Fixtures, perform the natural create Fixture -> create
     Group -> choose members -> create Scene -> delete selected Fixture workflow,
     verify persistence/reboot, rebuild the ARM64 offline package and update guides.
   - PASS: the complete corrected workflow passes on real WB8 and the replacement
     package is the only documented final installer.
   - Implementation checkpoint: final host/ARM64 suite is Confirmed; the reproducible
     `0.1.1` archive has SHA-256
     `42e440034dcacdfdb1cfff5ed3fb54bc2cc367b5a70200aaaba9a5924c09ef3d`,
     production binary SHA-256
     `ccece79bc02211e309a1e76bc600439d79f5316da8bd57a89a212a285157f03d`
     and `source_id=dev014c-final`. Clean-state WB8/reboot PASS is still pending.

The scope and order above are approved. A new fact that invalidates this plan must
be reported before changing the remaining substeps.

## Production items still Deferred

The fully offline bundle/install lifecycle and WB8 reboot acceptance are Confirmed.
The remaining Deferred production-distribution item is:

- registered production Art-Net OEM identity if not yet assigned.

Final engineering state:

```text
DEV-013 — full integration and final acceptance — Confirmed / FINAL PASS
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
