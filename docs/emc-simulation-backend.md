# Simulation EMC command reference

The EMC simulation supports energy and wavelength positioning and continuous
energy sweeps without monochromator hardware. It runs from the stand alone test IOC in this module.

## Contents

1. [Connection and syntax](#1-connection-and-syntax)
2. [Replies and errors](#2-replies-and-errors)
3. [Commands](#3-commands)
   - [3.1 Energy and status](#31-energy-and-status)
   - [3.2 Sweep](#32-sweep)
   - [3.3 Connection check](#33-connection-check)
4. [IOC shell commands](#4-ioc-shell-commands)
5. [Simulation behavior](#5-simulation-behavior)
  - [5.1 Unsupported commands](#51-unsupported-commands)

---

## 1. Connection and syntax

- The simulation server listens by default on TCP port **5001**.
- Send one command per line: the command name followed by its argument,
  for example `SPE 1004.0`.
- Simulation command names are not case sensitive. Leading and trailing
  whitespace is accepted.
- Numeric setters require exactly one finite, positive number. Use a decimal
  point, for example `SSV 2.5`; scientific notation is also accepted.
- Getters and action commands take no arguments. Missing or extra arguments
  return `f`.
- Requests may end with LF, CR or CR+LF. Blank lines are ignored by the server.
- Replies end with LF by default. The IOC can select CR+LF, CR or no terminator.

Energy is in **eV**, wavelength in **nm**, and speed in **eV/s**.

## 2. Replies and errors

| Reply | Meaning |
| --- | --- |
| `t` | Command accepted. A move or sweep may still be running. |
| `t <value>` | Success with a value. Floats have six decimals, for example `t 1004.000000`. |
| `f` | Unknown command, invalid argument, or command not allowed in the current state. |

### Status (GST)

`GST` returns a simple moving/ready value:

| Reply | Meaning |
| --- | --- |
| `t 1` | Energy is moving: target positioning, moving to the sweep start, or sweeping. |
| `t 0` | Energy is stationary: idle, sweep-ready, completed, or stopped. |

---

## 3. Commands

### 3.1 Energy and status

| Command | Argument | Reply | Description |
| --- | --- | --- | --- |
| `GST` | – | `t 0` or `t 1` | Ready or moving, see [Status](#status-gst). |
| `STO` | – | `t` | Stop at the current energy immediately and cancel sweep readiness. |
| `SPE` | energy, float (eV) | `t` | Move to the energy using the speed set by `SSV`. Poll `GST` for completion. |
| `GPE` | – | `t <float>` (eV) | Current energy. |
| `SPO` | wavelength, float (nm) | `t` | Convert wavelength to an energy target and move using `SSV` speed. |
| `GPO` | – | `t <float>` (nm) | Wavelength calculated from the current energy. |


### 3.2 Sweep

Typical sequence: set start, end and speed with `SSS`, `SSE`, `SSV`; send `SI`;
poll `GST` until it returns `t 0`; send `SR`; poll `GST` again for completion.
Read `GPE` or `GPO` during motion to follow the position.

| Command | Argument | Reply | Description |
| --- | --- | --- | --- |
| `SSS` | float (eV) | `t` | Set sweep start energy while stationary. |
| `SSE` | float (eV) | `t` | Set sweep end energy while stationary. |
| `SSV` | float (eV/s) | `t` | Set speed for all energy motion while stationary. Default: 1 eV/s. |
| `SGS` | – | `t <float>` (eV) | Sweep start energy. |
| `SGE` | – | `t <float>` (eV) | Sweep end energy. |
| `SGV` | – | `t <float>` (eV/s) | Configured speed. |
| `SI` | – | `t` | Prepare the sweep: move to its start energy, then become sweep-ready. |
| `SR` | – | `t` | Start the prepared sweep toward its end energy. |

#### Example

From the default 1000 eV position:

```text
SSS 1002
SSE 1008
SSV 2
SI
```

The setters and `SI` return `t`. Moving to 1002 eV takes one second. Poll `GST`
until it returns `t 0`, then send:

```text
SR
```

### 3.3 Connection check

The common server handles these exact lowercase lines before calling the
simulation dispatcher:

| Command | Argument | Reply | Description |
| --- | --- | --- | --- |
| `ping`, `health`, `test` | – | `OK` | Check that the server answers. |

Use the lowercase command alone, without surrounding spaces. Uppercase variants
are not supported by the simulation dispatcher and return `f`.

---

## 4. IOC shell commands

These commands run in the IOC shell, not over the EMC TCP connection. Load and
register the support first and configure access control with the existing
`emcAcl*` commands before starting the server.

The common command `emcIdleTimeout` shows the socket idle timeout; supply seconds
to change it, for example `emcIdleTimeout 1800`. It applies to every backend,
including mono MQ and simulation. The default, `0`, disables idle
expiry so a connection can stay silent for the entire beamtime. Changes apply
while running and survive stop/start and port changes. Enabling/shortening the
timeout can immediately close clients already idle longer than the new value.
Successful socket reads/writes refresh activity; waiting for backend work does
not. Negative/non-finite values and malformed input are rejected.
The separate 5-second deadline to finish a started request remains enabled.
Every server accepts at most 512 established connections and 64 per source IPv4
address; all clients through one jumphost share the latter allowance.

| Command | Argument | Description |
| --- | --- | --- |
| `emcSimStart` | port, integer (optional, default 5001) | Start the simulation server. Port 0 selects the default; otherwise use 1–65535. |
| `emcSimStop` | – | Stop the server and wait for its workers to finish. |
| `emcSimTerminator` | `lf`, `crlf`, `cr`, or `none` (optional) | Set the reply terminator. Without an argument, show the current setting. |
| `emcIdleTimeout` | finite seconds ≥ 0 (optional, default 0) | Common socket idle expiry for all backends; 0 disables it. Without an argument, show the setting. |
| `emcSimDebug` | level, integer ≥ 0 (optional, default 0) | Set simulation logging verbosity. |

Example:

```text
emcSimDebug 1
emcSimTerminator cr
emcSimStart 5001
```

Debug levels:

| Level | Output |
| ---: | --- |
| 0 | Quiet during normal operation; IOC startup/configuration errors remain enabled. |
| 1 | Accepted setters, state changes and rejected commands. |
| 2 or higher | Level 1 plus every simulation request and reply. |

Logs include commands and useful motion values such as energy, target, speed or
state. The debug level can be changed while the server runs.

To change the listening port, stop the running server first. The reply terminator
may be changed while it runs.

## 5. Simulation behavior

### Defaults

| Setting | Value |
| --- | --- |
| Initial energy | 1000 eV |
| Sweep start | 1000 eV |
| Sweep end | 1000 eV |
| Speed | 1 eV/s |
| Initial state | Idle, `GST` = `t 0` |

Set sweep endpoints before running a nonzero sweep.

### States

| Internal state | GST | Meaning |
| --- | --- | --- |
| `Idle` | `t 0` | Stationary without sweep readiness. |
| `MovingToTarget` | `t 1` | Moving after `SPE` or `SPO`. |
| `MovingToStart` | `t 1` | Preparing the sweep after `SI`. |
| `SweepReady` | `t 0` | Sweep start reached; `SR` is allowed. |
| `Sweeping` | `t 1` | Moving toward the sweep end after `SR`. |

## 5.1 Unsupported commands

These commands are not implemented in the simulation and return `f`:

| Command | Description |
| --- | --- |
| `GLE` | Read an error-history entry. |
| `OPN`, `CLO` | Compatibility open/close commands. |
| `SPL`, `SPD`, `SPS` | Set named integer, float or string parameters. |
| `GPL`, `GPD`, `GPS` | Read named parameters. |
| `UGG`, `USG`, `UGS`, `USS` | Read/set undulator gap or shift. |
| `UGF`, `USF`, `UGST` | Further undulator commands. |
| `DMEAS` | Read a measurement or parameter. |
| `:` | Packed energy reply. |
