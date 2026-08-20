# WB8 built-in RS-485 as DMX512 output from Linux userspace

**Document type:** reusable engineering reference  
**Status:** Confirmed on the tested WB8 configuration; portability notes included  
**Last reviewed:** 2026-08-20  
**Primary project:** DMXWB

## 1. Purpose

This document records the engineering work required to generate stable physical DMX512 from a built-in RS-485 port of a Wiren Board 8 controller from a Linux userspace process.

It is intentionally separate from the DMXWB product specification:

- `docs/TECHNICAL_SPEC.md` defines what DMXWB must do;
- this document explains how the physical transport was investigated, implemented and verified;
- project-specific limits such as `300 physical slots / fixed 44 Hz` are identified as DMXWB decisions, not as DMX512 protocol rules.

The document is intended to be reusable in other Linux/WB projects, but hardware capability must be re-verified on every new controller/UART/driver combination.

## 2. Evidence labels

The following labels are used throughout this document:

- **Confirmed** — measured on real hardware or covered by the committed implementation/test.
- **Portable principle** — generally useful Linux/serial design guidance, but still requires target validation.
- **WB8-specific observation** — observed on the tested Wiren Board configuration and must not be assumed for another UART.
- **DMXWB decision** — product policy, not a DMX512 or Linux requirement.

## 3. Tested environment

**Confirmed** acceptance environment:

| Item | Value |
|---|---|
| Controller | Wiren Board rev. 8.5.1 (T507) |
| Architecture | arm64 / aarch64 |
| OS | Debian 11 Bullseye |
| WB release | `wb-2606 stable` |
| Kernel | `6.8.0-wb160` |
| glibc | 2.31 |
| Physical port | built-in RS-485-1 |
| Linux alias | `/dev/ttyRS485-1` |
| Resolved UART during investigation | `ttyS2` |
| Production compiler | Bullseye `aarch64-linux-gnu-g++ 10.2.1` |
| Final tested artifact SHA256 | `670036f59558ad0a745c7d1f99764cb9257e08c71f42aa2f7853efc6ea83c1e0` |

The final fixed-profile acceptance report is `docs/DMX_FIXED_PROFILE_REPORT.txt`.

## 4. Physical DMX framing used by the implementation

The output line is configured as:

```text
250000 baud
8 data bits
no parity
2 stop bits
Start Code = 0x00
```

The transmitted frame is:

```text
BREAK
Mark After Break
Start Code 0x00
slot 1
slot 2
...
slot N
```

DMXWB deliberately uses conservative timing constants:

```text
BREAK hold = 120 us
MAB        = 20 us
```

These are implementation values used by the proven transport. For formal protocol compliance in another product, verify requirements against the current DMX512 / ANSI E1.11 specification rather than treating this document as the normative DMX standard.

One 8N2 byte at 250000 baud occupies 11 serial bits:

```text
11 / 250000 s = 44 us
```

So the payload wire time for `N` slots is approximately:

```text
(Start Code + N slots) * 44 us
```

## 5. Serial-port ownership matters

The built-in RS-485 port must have one owner.

Wiren Board normally uses `wb-mqtt-serial` for serial devices. If the same port is enabled in that driver while another process opens it for DMX, ownership is ambiguous and reliable operation cannot be assumed.

On the DMXWB acceptance stand, `/dev/ttyRS485-1` is permanently disabled in the WB Serial Device Driver Configuration, while `wb-mqtt-serial` itself remains running.

For another installation use one of these policies:

1. disable only the selected RS-485 port in the WB serial-driver configuration; or
2. stop the conflicting serial service while the custom application owns the port.

Do not silently share the UART between Modbus/`wb-mqtt-serial` and DMX.

Wiren Board documentation also warns about serial-driver conflicts when directly accessing `/dev/ttyRS485-*`.

## 6. First working method: baud-switch BREAK

### 6.1 Algorithm

The first hardware-proven userspace method was:

```text
configure 38400 8N2
write 0x00
wait until physical transmission completes
configure 250000 8N2
write Start Code + slots
wait until physical transmission completes
```

A zero byte at 38400 8N2 produces a sufficiently long low interval to act as BREAK.

### 6.2 What it proved

**Confirmed:**

- the built-in WB8 RS-485 port can physically drive a DMX fixture from Linux userspace;
- Start Code and RGBW channel mapping were correct;
- repeated close/reopen worked;
- no custom WB kernel/WBEC patch was needed for basic DMX output.

The DEV-003 hardware test confirmed `all-off`, red, green, blue, white and `all-on` on the real fixture.

### 6.3 Why it was not suitable for high-rate production output

Transport phase measurements showed that the expensive operation was not `write()` and not the baud reconfiguration itself. The dominant delays were the TTY drain waits.

Representative diagnostic results from the investigation:

```text
4-slot frame:
  drain BREAK ~12 ms average
  drain data  ~12 ms average
  send total  ~24 ms average, ~32 ms max

240-slot frame:
  send total  ~32 ms average, ~36 ms max
```

This was far above raw wire time and made reliable high refresh impossible.

**Portable principle:** when serial timing is unexpectedly slow, measure phases separately. A successful `write()` only means bytes entered the software/driver path; it does not prove the last stop bit has left the UART.

## 7. Why transmitter-empty information was important

The next diagnostic checked UART capabilities:

```text
TIOCGSERIAL
TIOCGRS485
TIOCSERGETLSR
```

On the tested UART:

- RS-485 configuration could be read;
- transmitter-empty status (`TIOCSERGETLSR` / `TIOCSER_TEMT`) was available;
- polling transmitter empty after writing a full 512-slot packet produced timing close to the physical wire time.

This was the key performance improvement: wait for actual UART transmitter empty instead of relying on the slow TTY drain behaviour observed on this target.

However, one more problem remained.

## 8. Failed fast attempt: hardware BREAK without manual DE

A candidate transport kept the UART at 250000 8N2 and used:

```text
TIOCSBRK
hold BREAK
TIOCCBRK
MAB
write payload
wait TEMT
```

Software timing was excellent, but the connected fixture did not accept the intended frame.

The important finding was:

> generating UART BREAK is not enough if the external RS-485 transceiver is not actually enabled during BREAK.

On this WB8 path, automatic RS-485 direction control was tied to normal transmit activity. BREAK generated while the driver was not in the transmit direction did not produce the complete physical DMX frame expected by the fixture.

**WB8-specific observation:** do not infer external RS-485 driver-enable state from UART BREAK state.

## 9. Proven fast method: manual DE + hardware BREAK + TEMT

The decisive method was to take explicit ownership of RS-485 direction while DMX is being generated.

### 9.1 Capability check

The production transport first probes the opened port.

Required capabilities for the fast path are:

```text
TIOCGRS485 / TIOCSRS485
manual RTS control through TIOCMBIS / TIOCMBIC
TIOCSBRK / TIOCCBRK
TIOCOUTQ
TIOCSERGETLSR with TIOCSER_TEMT
```

Capability is detected on the actual open file descriptor. It is not inferred from a WB model name.

### 9.2 Save original RS-485 state

At open:

```text
TIOCGRS485 -> save struct serial_rs485
```

The saved state must survive until close/recovery cleanup.

### 9.3 Disable automatic RS-485 direction control

The production fast path clears:

```text
SER_RS485_ENABLED
```

and writes the modified configuration with `TIOCSRS485`.

The application then controls the transmitter direction explicitly using RTS.

Linux documents `struct serial_rs485`, `TIOCGRS485` and `TIOCSRS485` as the userspace interface for supported serial drivers.

### 9.4 Per-frame algorithm

The proven production sequence is:

```text
1. RTS/DE ON
2. TIOCSBRK
3. hold >= 120 us
4. TIOCCBRK
5. wait MAB >= 20 us
6. write Start Code 0x00 + active slots
7. poll until:
       TIOCOUTQ == 0
       AND
       TIOCSERGETLSR has TIOCSER_TEMT
8. RTS/DE OFF
```

DMXWB uses monotonic sleep for BREAK/MAB and a small polling sleep while waiting for TEMT.

### 9.5 Why both `TIOCOUTQ` and TEMT are checked

`TIOCOUTQ == 0` tells us that the driver output queue is empty.

`TIOCSER_TEMT` tells us that the UART transmitter itself is empty.

For half-duplex RS-485 direction control, DE must not be removed merely because userspace or a driver queue is empty. It should remain asserted until the UART has finished the physical stop bits.

**Portable principle:** distinguish software-queue empty from transmitter physically empty.

## 10. Cleanup and failure semantics

Any fast-path error performs best-effort physical cleanup:

```text
TIOCCBRK
RTS/DE OFF
```

On close:

```text
fast physical cleanup
restore original serial_rs485
close fd
```

The original RS-485 configuration is also restored when the continuous-output recovery state machine closes a failed port before a reopen attempt.

This avoids leaving:

- BREAK asserted;
- DE asserted;
- the port permanently switched out of its original RS-485 mode.

## 11. Compatibility fallback

If the required fast-path ioctls are unavailable, the committed transport retains the DEV-003 compatibility method:

```text
38400 8N2 -> 0x00 -> drain
250000 8N2 -> Start Code + slots -> drain
```

This fallback is useful because support for Linux serial ioctls varies by UART driver.

It is **not** evidence that an unknown fallback target can sustain the DMXWB fixed `300 slots / 44 Hz` production profile.

The proven high-rate profile is tied to the fast path.

## 12. Continuous-output scheduler

Physical transmission is not triggered by changes in control values.

The output loop runs on an absolute frame-start grid:

```text
T0
T0 + period
T0 + 2 * period
...
```

A whole immutable snapshot is acquired only on a frame boundary.

If the scheduler is late, missed deadlines are counted and the same absolute grid is preserved instead of changing to:

```text
send
sleep(period)
send
sleep(period)
```

which would accumulate send-time drift.

## 13. Why the final DMXWB profile is 300 slots / 44 Hz

This is a **DMXWB product decision**, not a limitation of DMX512.

The internal DMX/network capacity remains 512 channels, but the physical WB8 output is limited to 300 slots and uses a fixed 44 Hz cadence.

### 13.1 Evidence before changing the core

Production fast-transport acceptance showed:

| Test | Result |
|---|---|
| 512 slots / 30 Hz / 60 s | PASS, zero missed deadlines |
| 240 slots / 44 Hz / 30 s | PASS, zero missed deadlines |
| 512 slots / requested 44 Hz | correctly rejected by the earlier measured-feasibility implementation |

A later strict test of:

```text
512 slots / 40 Hz / 60 s
```

produced:

```text
frames_sent: 2399
missed_deadlines: 1
max_send_us: 23932
visual: PASS
```

Although visually acceptable, one missed deadline was enough to reject `512/40` as a guaranteed fixed profile.

Two exploratory runs of:

```text
300 slots / 44 Hz / 60 s
```

both produced `2640/2640` frames, zero missed deadlines and no visible flicker. The worse observed send time between those two runs was `17.689 ms`, leaving about 5 ms before the `22.727 ms` frame period.

### 13.2 Final post-change proof

After the production core was changed to enforce the fixed profile, the new ARM64 binary was tested again:

```text
physical slots:          300
fixed refresh:           44 Hz
duration:                60 s
frames_sent:             2640
open_failures:           0
send_failures:           0
recoveries:              0
missed_deadlines:        0
max_send_us:             16407
max_transport_overhead:  3023 us
visual result:           PASS
all-off/reopen:          PASS
```

This is the result that confirms the current DMXWB production core.

## 14. What to re-test on another WB8 model or Linux board

Do not copy only the final ioctl sequence and assume success.

Use this checklist:

1. Identify the actual device behind `/dev/ttyRS485-*`.
2. Ensure no other service owns the port.
3. Verify exact `250000 8N2` support.
4. Read `serial_rs485` with `TIOCGRS485`.
5. Verify `TIOCSRS485` can disable automatic direction control.
6. Verify manual RTS really controls the external RS-485 DE on the physical connector.
7. Verify `TIOCSBRK/TIOCCBRK` produces BREAK while DE is asserted.
8. Verify `TIOCSERGETLSR/TIOCSER_TEMT` is supported and reflects the real UART transmitter.
9. Measure a worst-case full intended frame, not only a short 4-slot frame.
10. Run a long continuous test with missed-deadline accounting.
11. Check real fixtures for colour correctness and flicker.
12. Verify close/reopen and restoration of the original RS-485 configuration.
13. Re-test after kernel/driver changes.

## 15. Common mistakes

### Treating `write()` completion as physical completion

Wrong assumption:

```text
write() returned -> frame is on the wire
```

Correct model:

```text
write() returned -> data accepted by software/driver
TEMT             -> UART transmitter physically empty
```

### Generating BREAK while DE is inactive

UART BREAK and RS-485 bus BREAK are not equivalent if the transceiver driver is disabled.

### Leaving automatic and manual direction control active simultaneously

If userspace is manually controlling DE, explicitly define who owns direction control. Mixing two independent direction-control mechanisms makes timing and cleanup ambiguous.

### Dropping DE before the final stop bits

This can truncate the physical frame even when buffers look empty.

### Testing only short frames

A 4-slot fixture test proves signal correctness, not worst-case frame timing.

### Calling a visually acceptable run a timing PASS

The `512/40` experiment looked fine but had one missed deadline. Timing acceptance must be based on diagnostics as well as observation.

## 16. Safety notes

- Follow normal RS-485 wiring, polarity and termination requirements for the installation.
- Do not connect an earth-referenced oscilloscope ground directly to RS-485 A/B without confirming an electrically safe measurement arrangement.
- If using a logic analyser or oscilloscope, verify the measurement interface and isolation before connecting it to the bus.
- A production application should restore the original serial configuration even after errors.

## 17. Relevant DMXWB files

Current implementation and evidence:

```text
src/dmx_transport_linux.cpp
include/dmxwb/dmx_transport.hpp
src/dmx_output.cpp
include/dmxwb/dmx_output.hpp
docs/DEV003B_HARDWARE_REPORT.txt
docs/DEV004B_FAST_TRANSPORT_REPORT.txt
docs/DMX_FIXED_PROFILE_REPORT.txt
tools/wb8/run_fixed_dmx_profile_acceptance.sh
```

## 18. External references

Linux RS-485 userspace API:

- https://docs.kernel.org/driver-api/serial/serial-rs485.html
- fallback mirror/versioned documentation: https://www.kernel.org/doc/html/v6.1/driver-api/serial/serial-rs485.html

Wiren Board RS-485 documentation:

- https://wirenboard.com/wiki/RS-485
- https://wirenboard.com/wiki/Wiren_Board_8.5
- https://wirenboard.com/wiki/Доступ_к_порту_RS-485_контроллера_Wiren_Board_с_компьютера/en

Formal DMX protocol requirements should be checked against the current ANSI E1.11 / DMX512-A specification for any new product or compliance claim.
