# ElderberryMC (EMC)

ElderberryMC is an EPICS support module providing a TCP server for the EMC
command protocol. The server handles connections, access control, protocol
limits and reply terminators; a dispatcher (the backend) executes commands.
The included monochromator simulation supports energy and wavelength
positioning and continuous energy sweeps without hardware.

**Status: pre-0.1.0.** APIs and defaults may change. The simulation is the
monochromator backend included in this repository. The hardware backend and
planned gateway have separate availability:

| Backend | Purpose | Availability |
| --- | --- | --- |
| [Monochromator simulation](docs/emc-simulation-backend.md) | Energy/wavelength moves and continuous sweeps without hardware or mono MQ. | Included. |
| [Mono MQ backend](docs/emc-mq-backend.md) | Connect existing EMC clients to the HZB mono application, controlling real hardware or its digital twin. | HZB-internal; implementation outside this repository. |
| EMC-to-EPICS gateway | Connect EMC clients to EPICS through the actuator framework. | Planned public release. |

The MQ references describe the separate mono application backend. Its status
word, parameter commands and sweep handling differ from the simulation.

## Features

### EMC protocol server

- Human readable EMC text protocol over TCP, default port **5001**.
- Requests may end with LF, CR or CR+LF; blank lines are ignored.
- Configurable reply terminator: LF (default), CR+LF, CR or none.
- Connection check: the lowercase lines `ping`, `health` and `test`
  are answered `OK` by the server for every backend.
- One shared server core (`EmcServer`) for all backends providing limits, access
  control and IOC shell settings.
- Selectable IPv4 listener address with `emcBindAddress` (default `0.0.0.0`,
  all local addresses). Invalid settings and failed bind block the start.

### Access control

- IPv4 access control list checked when a client connects, shared by all EMC
  services in the IOC.
- Modes: `allowlist`, `denylist`, `default-deny`, `allow-all`.
- Rules for single addresses or CIDR networks (`emcAclAllow 192.168.10.0/24`).
- Safe by default: no client can connect until allowed.
  E.g. a typo in an ACL command stops the server from starting.
- Diagnosis: `emcAclShow` and `emcAclCheck` show the configuration
  and test an address.

### Protection against misbehaving clients

- At most 512 connections per server and 64 per source IPv4 address.
- Requests are limited to 4096 bytes and must be completed within 5 seconds.
- Requests containing NUL bytes close the connection before they reach a backend.
- Pending replies are limited to 64 KiB per client and 4 MiB per server.
- Request rate limit per connection (default 500 commands/s, burst 20) and a
  limit of 8 outstanding commands.
  IOC-Shell configuration:  `emcRequestRate`, `emcRequestBurst` and `emcMaxOutstandingRequests`.
- Optional idle timeout with `emcIdleTimeout` (default off, so connections can
  stay open for a whole beamtime).
- No SIGPIPE: abrupt disconnects cannot terminate the IOC.
- Limit violation are logged.

### Monochromator simulation

- Standalone simulation of energy and wavelength positioning and continuous
  energy sweeps.
- Supports `GST`, `STO`, `SPE`, `GPE`, `SPO`, `GPO` and the sweep commands
  `SSS`, `SSE`, `SSV`, `SGS`, `SGE`, `SGV`, `SI`, `SR`.
- IOC shell commands to start, stop, set the terminator and the debug level
  (`emcSimStart`, `emcSimStop`, `emcSimTerminator`, `emcSimDebug`).
- Includes a test IOC (`testIoc`) that starts the simulation.

### Build and quality

- `make test`, unit testing, no hardware is needed.
- `make cppcheck`, `make clang-tidy` and `make checkall` targets for static analysis.

## Dependencies

- **EPICS Base 3.14.12.8 or later:** Set its path in `configure/RELEASE`.
- **C++14 compiler:** GCC or Clang.
- **GNU make and Perl:** required by the EPICS build system.

Optional development tools are **cppcheck**, **clang-tidy** and **cscope**.
Use Netcat (`nc`) or `telnet` for manual TCP testing.

## Getting started

### Build and test

```sh
git clone https://github.com/hz-b/emc.git
cd emc
```

Create `configure/RELEASE` so `EPICS_BASE` points to your Base installation:

```make
EPICS_BASE=/opt/epics/base
```

Build the module and bundled test IOC, then run the tests:

```sh
make
make test
```

The unit and loopback tests need no monochromator hardware.

### Start the embedded simulation IOC

From the module root, launch the test IOC:

```sh
make -C testIoc/ run
```

#### **IOC shell** Example:

```text
dbLoadDatabase("dbd/elderberryMCTestIoc.dbd")
elderberryMCTestIoc_registerRecordDeviceDriver(pdbbase)
iocInit()
emcAclMode default-deny
emcAclAllow 127.0.0.1
emcBindAddress "127.0.0.1"
emcSimTerminator lf
emcSimStart 5001
```

### Manual test

In another terminal on the same host, run:

```sh
telnet 127.0.0.1 5001
```

```text
ping
OK
GST
t 0
GPE
t 1000.000000
```

Alternatively, connect with `nc 127.0.0.1 5001` and enter the same commands.
The `ping` reply checks the server connection, not hardware readiness.

Use `emcSimStop` in the IOC shell to stop the listener. Set `emcSimDebug 1`
for simulation state changes and rejected commands, or `emcSimDebug 2` for
every simulation request and reply. See the
[simulation reference](docs/emc-simulation-backend.md#4-ioc-shell-commands)
for startup, shutdown, terminator and debug settings.

## Protocol basics

Send one EMC command per line over TCP, normally on port **5001**. Requests
may end with LF, CR or CR+LF; blank lines are ignored. 
The exiting legacy VxWorks implementation supports only `cr`.
Replies end with LF by default, independently of the request terminator. 
`emcSimTerminator` can select `lf`, `crlf`, `cr` or `none` for simulation replies.


| Simulation reply | Meaning |
| --- | --- |
| `t` | Command accepted; motion may still be running. |
| `t <value>` | Command succeeded and returned a value. |
| `f` | Unknown command, invalid argument, or command not allowed in the current state. |
| `t 0` from `GST` | Energy is stationary. |
| `t 1` from `GST` | Energy is moving. |

Energy is in **eV**, wavelength in **nm**, and sweep velocity in **eV/s**.
For a move, send `SPE <energy>` or `SPO <wavelength>` and poll `GST` for
completion. For a sweep, set `SSS`, `SSE` and `SSV`, send `SI`, wait for
`GST` to return `t 0`, then send `SR` and monitor completion.

See the [simulation command reference](docs/emc-simulation-backend.md) for
arguments, examples and unsupported commands. The MQ backend uses a
[different GST status word](docs/emc-mq-backend.md#status-word-gst).
In the simulation, `SPE`/`SPO` replace an active sweep with a target move;
the MQ backend instead follows the [deferred-move rules](docs/sweep-set-energy.md). The legacy
[`:` binary energy command](docs/colon-command.md) is not implemented by
the current server.

## Integration into an existing IOC

Build this module against the same EPICS Base and architecture as your IOC.
Add the module path to the IOC application's `configure/RELEASE`, using
`EMC` as the module variable:

```make
EMC=/path/to/emc
```

In the IOC's source Makefile, add the support DBD and library. Replace
`myIoc` with your IOC product name, and place `elderberryMC` before the Base
IOC libraries:

```make
myIoc_DBD += elderberryMC.dbd
myIoc_LIBS += elderberryMC
myIoc_LIBS += $(EPICS_BASE_IOC_LIBS)
```

## Access control and network configuration

- ACLs match **client source addresses**
- `emcBindAddress` selects the **local server address**. 
- The default ACL mode is `default-deny` with no rules, which refuses all clients. 
- The listener is IPv4-only.

| ACL mode | Accepted clients |
| --- | --- |
| `allowlist` | Clients matching an allow rule. Only allow rules are valid. |
| `denylist` | Clients not matching a deny rule. Only deny rules are valid. |
| `default-deny` | The closest matching rule wins. If rules conflict or not match, access is denied. |
| `allow-all` | All clients. No rules may be configured. |

Configure ACLs with `emcAclMode`, `emcAclAllow` and `emcAclDeny`. Rules use
numeric IPv4 addresses or CIDR networks, for example `192.168.10.0/24`.
CIDR rules must use a network address with host bits cleared.
`emcAclShow` displays the configuration and service state;
`emcAclCheck 192.168.2.42` tests a client address without connecting.

### Allowing network clients

After stopping all EMC services, use your client network and an IP assigned
to the IOC, for example:

```text
emcAclClear
emcAclMode default-deny
emcAclAllow 192.168.10.0/24
emcBindAddress "192.168.10.20"
emcSimStart 5001
```

See the [listener address reference](docs/emc-limits-and-timeouts.md#listener-address)
for address validation, shared settings and listener diagnostics.

## Development

Run these targets from the module root:

| Command | Purpose |
| --- | --- |
| `make` | Build the library, test programs and embedded test IOC. |
| `make test` | Run the unit and loopback tests after building. |
| `make distclean` | Clean the module and embedded test IOC. |
| `make cppcheck` | Run cppcheck on library and test IOC sources. |
| `make clang-tidy` | Run Clang analyzer, bugprone, performance and portability checks on those sources. |
| `make checkall` | Run both analysis tools, continuing if one fails. |
| `make cscope` | Build a source-navigation database including EPICS Base sources. |


Override tool paths or flags when needed:

```sh
make checkall CPPCHECK=/path/to/cppcheck CLANG_TIDY=/path/to/clang-tidy
make checkall CHECK_STD=c++14
```

`CPPCHECK_FLAGS` and `CLANG_TIDY_FLAGS` override the default check options.


## Documentation

| Reference | Scope |
| --- | --- |
| [Simulation backend](docs/emc-simulation-backend.md) | Included backend: syntax, commands, replies, IOC shell settings and motion behavior. |
| [Listener address, limits and timeouts](docs/emc-limits-and-timeouts.md) | Common server settings for all backends. |
| [Mono MQ backend](docs/emc-mq-backend.md) | External HZB backend: hardware status, commands and named parameters. |
| [Setting the energy during a sweep](docs/sweep-set-energy.md) | External MQ backend: deferred `SPE`/`SPO` behavior during onfly sweeps. |
| [The `:` binary energy command](docs/colon-command.md) | Legacy VxWorks protocol; reference for a future implementation. |

## Contributing

Keep changes focused, add/update unit tests, build and run:

```sh
make
make test
make checkall
```
## License

Licensed under the [MIT license](LICENSE).

Copyright 2026 Helmholtz-Zentrum Berlin für Materialien und Energie GmbH.
