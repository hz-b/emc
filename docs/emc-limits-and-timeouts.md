## Protocol limits and timeouts

These limits apply to every backend served by `EmcServer`.

### Listener address

The IOC shell command `emcBindAddress` selects the local IPv4 address. 

```text
emcBindAddress                    # show the common setting
emcBindAddress "0.0.0.0"          # all local IPv4 addresses (default)
emcBindAddress "127.0.0.1"        # IPv4 localhost only
emcBindAddress "192.168.2.40"     # only this local IOC address (example)
```

Use a numeric IP assigned to the IOC. Hostnames, CIDR ranges/prefixes, IPv6,
leading zeros, whitespace, multicast and `255.255.255.255` are rejected.
Without an argument, emcBindAddress shows the current setting. 
A wrong address, or one the server can’t use, 
stops new servers from starting until valid addresses are configured. 
Changes apply to new starts only; stop and restart every listener that should move:

```text
emcSimStop
emcBindAddress "192.168.10.20"
emcSimStart 5001
```

When several backends run, each keeps its previous address until restarted.
Put it in the IOC startup script to preserve it across IOC restarts.

The bind address selects a server destination address, not permitted client
source addresses. ACL enforcement remains active, and firewall rules should
restrict unwanted routed access. 
Selecting the client-network IP does not restrict the backend's EPICS/device communication.
To test on the IOC itself, connect from the address you set: 
127.0.0.1 only works if the setting is 127.0.0.1 or 0.0.0.0
The listener is IPv4-only.

The command is registered once by `emcServerLimitsRegister()` in the shared
server source. `emcCoreRegister()` already calls that registrar. Other IOC
wrappers may call it too. No backend-specific bind command or extra startup argument is required.

Successful starts, invalid configuration and bind failures are logged.
Verify the actual listener on Linux:

```sh
ss -ltnp | grep 5001
```

Then test the intended address from the jumphost/diagnostic clients and verify
no listener is reachable on the IOC's excluded addresses. Routed access to
the selected IP remains a firewall/ACL concern.

### Request and reply limits

| Limit | Default | Behavior when exceeded |
| --- | --- | --- |
| Request length | 4096 bytes per command, excluding CR/LF | Close the client before storing the excess bytes. The oversized command is not dispatched. |
| Pending replies per client | 64 KiB, including terminators | Close the client before appending the reply. |
| Pending replies per server | 4 MiB across all clients of one server instance | Close the client whose next reply would exceed the total budget. |

Request length includes spaces. Several valid commands received together
are checked individually. NUL bytes are rejected separately, as described below.

Only replies waiting for a slow client count toward the limit.
Sent replies and replies to disconnected clients stop counting.

### Request rate and outstanding commands

A client that sends too many commands too fast is disconnected.
These are IOC shell settings and they apply to all backends.

| Setting | IOC shell command | Default | Disable |
| --- | --- | --- | --- |
| Commands per second, per connection | `emcRequestRate` | 20 | `0` |
| Commands allowed at once (burst), per connection | `emcRequestBurst` | 10 | not possible, minimum 1 |
| Outstanding commands per connection | `emcMaxOutstandingRequests` | 8 | `0` |

```text
emcRequestRate                  # show the setting
emcRequestRate 100              # allow 100 commands per second
emcRequestBurst 20              # allow short bursts of up to 20 commands
emcMaxOutstandingRequests 16    # allow up to 16 commands waiting for the backend
emcRequestRate 0                # turn the rate limit off
```

Without an argument each command shows the current setting. Invalid values
(negative, non-numeric, a burst of 0) are rejected and the old value stays.

**How the rate limit works.** Every connection has an allowance of commands.
A new connection starts with the full burst (default 10 commands). Each
command uses one, and the allowance refills at the configured rate (default
500 per second) up to the burst size. A client that sends a command while its
allowance is empty is disconnected.
A client can send emcRequestBurst command at once and the at a rete of emcRequestRate.

**How the outstanding limit works.** A command is outstanding from the moment
the server receives it until the backend has processed it. A client that
sends a new command while it already has the maximum number outstanding
(default 8) is disconnected. Clients that wait for each reply before sending
the next command never reach this limit (recommended).

What counts:

- Each command line counts, also several commands received in one TCP read,
  and also commands that a request filter rejects.
- Blank lines do not count.

When a limit is exceeded, the client gets no error reply; the connection is
closed. 

Changes apply immediately to all running servers and existing connections,
and survive server stop/start. A lower outstanding limit applies from the
next command; work already accepted is not cancelled. Put the settings in
the IOC startup script.

### Request text and control bytes

Requests are line-delimited text, terminated by CR, LF or CRLF. Blank lines
are ignored. NUL (`0x00`) is forbidden anywhere in received request data.
Other bytes are passed through without removal or rewriting; command syntax,
argument syntax and acceptance of other control bytes are handled by the dispatcher.

If a client sends a NUL byte, the server closes the connection at once and drops everything in that data.

### Timeouts

| Timeout | Default | Behavior |
| --- | --- | --- |
| Incomplete request | 5 seconds from its first command byte | Close the client if the terminating CR/LF does not arrive in time. |
| Idle connection without an incomplete request | None | The connection remains open. |
| Backend command execution | No server-enforced deadline | The server waits for the dispatcher to return. Shutdown also waits for an in-flight call. |

The incomplete-request deadline is absolute: sending additional fragments
does not extend it. A new request starts its own deadline.

### Configuration

The limits are compile-time constants in `elderberryMCApp/src/emc_server.h`:

- `EMC_MAX_REQUEST_BYTES`
- `EMC_REQUEST_TIMEOUT_SECONDS`
- `EMC_MAX_CLIENT_REPLY_BYTES`
- `EMC_MAX_TOTAL_REPLY_BYTES`

Changing these values requires rebuilding the module. Pending-reply limits
include the configured reply terminator: LF, CRLF, CR or none.

### Diagnostics and tests

Limit violations produce debug messages:

- `EMC REQUEST TOO LONG`
- `EMC REQUEST TIMEOUT`
- `EMC REPLY LIMIT`
- `EMC INVALID REQUEST` (NUL byte; connection closed before buffering)
- `EMC REQUEST RATE LIMIT`
- `EMC OUTSTANDING REQUEST LIMIT`

Reply-limit messages identify the client, pending bytes, incoming reply
size and the budget exceeded. Connection-close messages report released
bytes and the remaining total.

Set `emc_server_debug` to 1 to also log configured limits and queued replies.

