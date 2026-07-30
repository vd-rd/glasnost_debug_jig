# Glasnost Jig — USB Wire Protocol (v1)

## USB device shape

The jig enumerates as a single composite USB device with **two independent
CDC-ACM interfaces** (via `esp_tinyusb`, `CONFIG_TINYUSB_CDC_COUNT=2`):

- One interface is the **UART passthrough** — a byte-transparent bridge to
  the DUT's console UART. Baud rate is *not* a jig command: it tracks
  whatever baud the host sets on that virtual COM port (CDC `SetLineCoding`),
  propagated live to the physical UART. Open it with `picocom`/`minicom`/
  `screen` like any serial console.
- The other is the **control channel** — a line-based ASCII command/response
  protocol, described below.

### Port discovery (no fixed ordering)

Two identical-looking `/dev/ttyACM*` (or `COMn`) ports appear per jig, and
`esp_tinyusb`'s Kconfig does not let per-CDC-interface strings distinguish
them at the descriptor level. **Do not assume enumeration order.** Discover
the control port by probing:

1. Enumerate candidate serial ports (optionally filtered by VID/PID if the
   jig uses a distinct one).
2. Open each at any baud, send `PING\n`, wait up to ~300 ms for a line.
3. The port that replies `OK PONG` is the control channel. The other port
   belonging to the same USB device is the UART passthrough.

This is what `host/jigctl` implements — treat it as the reference behavior.

## Control channel framing

- ASCII text, one command per line, terminated by `\n` (a trailing `\r` is
  tolerated and stripped).
- Commands are whitespace-separated tokens: `VERB [ARG ...]`. The verb is
  matched case-insensitively; arguments are verb-defined.
- Exactly one response line per command:
  - `OK[ <data>]` on success.
  - `ERR <REASON>` on failure, `REASON` a single upper-snake-case token
    (e.g. `UNKNOWN_COMMAND`, `NOT_WIRED`, `BAD_ARG`).
- The channel is strictly request/response in v1 — the jig never sends a
  line unprompted. (An `EVT ...` line prefix is reserved for a future
  asynchronous-event mode; hosts should treat any line not matching a
  request they just sent as ignorable rather than erroring.)
- Max line length 128 bytes; longer input is an error (`ERR LINE_TOO_LONG`)
  and the parser resyncs on the next `\n`.

## Command set (v1)

| Command | Response | Notes |
|---|---|---|
| `PING` | `OK PONG` | liveness / port discovery |
| `VERSION` | `OK glasnost-jig <fw_ver> idf <idf_ver>` | |
| `POWER ON` | `OK POWER ON` | enable DUT power switch |
| `POWER OFF` | `OK POWER OFF` | disable DUT power switch |
| `POWER CYCLE [ms]` | `OK POWER CYCLE` | off, wait `ms` (default 1000), on |
| `POWER STATUS` | `OK POWER <ON\|OFF>` | last commanded state |
| `RESET [ms]` | `OK RESET` | pulse dedicated reset line low/active for `ms` (default 200), if wired |
| `UART BAUD <n>` | `OK UART BAUD <n>` | manual override; normally the host setting line coding on the passthrough port is enough |
| `UART STATUS` | `OK UART BAUD <n>` | |
| `STATUS` | `OK POWER=<ON\|OFF> UART_BAUD=<n> UPTIME=<sec>` | one-shot summary |
| `SPI ...` | `ERR NOT_IMPLEMENTED` | reserved namespace, see below |
| `I2C ...` | `ERR NOT_IMPLEMENTED` | reserved namespace, see below |
| anything else | `ERR UNKNOWN_COMMAND` | |

Any command touching a GPIO function that isn't wired on the current board
revision returns `ERR NOT_WIRED` rather than silently no-op'ing (e.g. `RESET`
when no dedicated reset line is configured in `board_config.h`).

## Reserved future namespaces

`SPI` and `I2C` are reserved verb prefixes for peripheral-bus test commands
(bit-bang or hardware master probing on the DUT's bus) once hardware pin
assignments for those signals exist. They intentionally return
`ERR NOT_IMPLEMENTED` today instead of being absent, so host tooling can
already special-case "not yet supported" vs "unknown command."

## Versioning

This is protocol v1. If the command set changes incompatibly, `VERSION`
gains a `proto <n>` field and this document is updated in lockstep — no
separate version-negotiation handshake in v1.
