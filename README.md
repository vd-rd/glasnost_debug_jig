# glasnost_debug_jig

An ESP32-S3 based USB jig for bring-up debugging of the owner's custom SBC
boards (`sbc_at91sam9g10/g20/g25`, `sbc_allwinner_a13/a20/a33/F133/F1C200s/v3s`,
and others, one level up in `/home/vadim/storage/hardware/`). It plugs into a
DUT's debug header and gives the host two things over a single USB
connection: a transparent passthrough to the DUT's console UART (for
u-boot/kernel/distro serial console work), and a small command channel to
power the DUT on/off, cycle power, and pulse a reset line — the stuff you'd
otherwise do by hand with a bench supply and a jumper wire while iterating on
a board that doesn't boot yet. `SPI`/`I2C` verbs are reserved in the protocol
for future bus-level probing but are **not implemented** — see below.

## Hardware is not part of this repo (yet)

The schematic/PCB, connector strategy, and which physical signal goes to
which ESP32-S3 GPIO are being designed separately by the board owner. This
repo is firmware + host tooling only. Every pin assignment in
[`firmware/main/board_config.h`](firmware/main/board_config.h) is an
explicit placeholder (chosen only to avoid strapping pins, the native-USB
D+/D-, and the console UART pins) and will change once the real hardware
exists. Notably, with the current placeholders the reset line
(`BOARD_RESET_GPIO`) is left unwired (`RESET` returns `ERR NOT_WIRED`),
while the power-switch and DUT UART pins are wired to placeholder GPIOs.

See the "Hardware-boundary notes" section of
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for known constraints the
eventual hardware design needs to account for (safe-off pull on the power
switch line, USB power descriptor).

## Layout

- `firmware/` — ESP32-S3 firmware (ESP-IDF), builds via podman, no local IDF
  install required.
- `host/` — `jigctl`, a Python CLI for driving the jig from the host side.
- `docs/` — [`ARCHITECTURE.md`](docs/ARCHITECTURE.md) (firmware design/module
  layout) and [`PROTOCOL.md`](docs/PROTOCOL.md) (the USB wire protocol spec).

## Releases

Tagged releases (see [GitHub Releases](https://github.com/vd-rd/glasnost_debug_jig/releases))
attach two prebuilt binaries so you don't need podman/ESP-IDF or a Python
environment just to try the jig:

- `glasnost_jig-<version>-esp32s3-merged.bin` — bootloader + partition table
  + app merged into one flashable image (via `idf.py merge-bin`). Flash it
  directly with `esptool.py --chip esp32s3 write_flash 0x0 <file>`, no build
  step required.
- `jigctl-<version>-linux-x86_64` — standalone PyInstaller build of
  `host/jigctl.py`; `chmod +x` and run it directly, no `pip install` needed.

Building from source (below) is still the way to go if you're changing the
firmware or want other platforms for `jigctl`. See
[`docs/RELEASING.md`](docs/RELEASING.md) for how releases are cut.

## Quickstart

### Firmware: build / flash / monitor

Target is ESP32-S3 (needs real USB-OTG silicon for the two-CDC composite
device; C3/C6/H2's fixed-function USB-Serial-JTAG peripheral can't do this).
Built against ESP-IDF v5.5 via podman (`docker.io/espressif/idf:release-v5.5`),
using `espressif/esp_tinyusb` v2.2.1 (pinned in
`firmware/main/idf_component.yml`) for the USB CDC stack.

```sh
firmware/scripts/build.sh                       # set-target esp32s3 + build
firmware/scripts/flash.sh   /dev/ttyUSB0         # flash over the given device
firmware/scripts/monitor.sh /dev/ttyUSB0         # idf.py monitor on the given device
firmware/scripts/menuconfig.sh                   # idf.py menuconfig
```

`flash.sh` and `monitor.sh` both require the serial device path as their
first argument (the download/JTAG UART used for flashing — not one of the
jig's own USB-CDC ports); `flash.sh` forwards any extra arguments straight
to `idf.py flash`. None of the scripts assume a local IDF install — they
bind-mount `firmware/` into the espressif/idf container and run `idf.py`
there.

### Host: jigctl

```sh
pip install -r host/requirements.txt      # just pyserial
python3 host/jigctl.py <command> ...
# or, installed as a console script:
pip install host/
jigctl <command> ...
```

The jig enumerates as two `/dev/ttyACM*` (or `COMn`) ports and **their
ordering is not meaningful** — `esp_tinyusb` can't give the two CDC
interfaces distinct descriptor strings, so nothing at the OS level tells you
which port is the control channel and which is the DUT UART passthrough.
`jigctl` handles this itself: by default it enumerates candidate serial
ports, opens each, sends `PING`, and whichever one answers `OK PONG` is the
control port; the other port sharing the same USB topology (same serial
number, or same USB location minus the interface suffix) is inferred as the
UART passthrough. Use `jigctl list` to see what it discovered, `--port` to
bypass discovery entirely, and `--device` / `--vid` / `--pid` to disambiguate
when more than one jig is plugged in.

Subcommands (`python3 host/jigctl.py --help` and `<subcommand> --help` for
exact flags):

```
ping                          send PING, print reply
version                       print firmware/IDF version
power {on,off,cycle,status}   power on / off / cycle [--ms N, default 1000] / status
reset [--ms N]                pulse reset line (default 200 ms; ERR NOT_WIRED on current placeholders)
uart-baud <baud>              set DUT UART baud
uart-status                   read current DUT UART baud
status                        one-shot POWER/UART_BAUD/UPTIME summary
list                          discover attached jigs, print control/passthrough port pairing
monitor [--baud N] [--uart-port PATH]   interactive terminal bridge to the DUT UART passthrough port
```

(That's a paraphrase of the real `--help` output, not the literal text —
run `--help` for the authoritative flag list.)

## Control-channel command set

The jig's control port (`CDC_ACM_1`) speaks a line-based ASCII
request/response protocol: one command per line in, exactly one `OK ...` or
`ERR REASON` line back. Full framing rules and the canonical command table
are in [`docs/PROTOCOL.md`](docs/PROTOCOL.md); as implemented today
(`firmware/main/cmd_parser.c`):

| Command | Response | Notes |
|---|---|---|
| `PING` | `OK PONG` | |
| `VERSION` | `OK glasnost-jig <fw_ver> idf <idf_ver>` | |
| `POWER ON` / `POWER OFF` | `OK POWER ON`/`OFF` | `ERR NOT_WIRED` if the power switch pin isn't configured |
| `POWER CYCLE [ms]` | `OK POWER CYCLE` | off, wait `ms` (default 1000, capped at 60000), on; non-blocking on the jig side |
| `POWER STATUS` | `OK POWER <ON\|OFF>` | last commanded state |
| `RESET [ms]` | `OK RESET` | pulses the dedicated reset line (default 200 ms, capped at 60000); `ERR NOT_WIRED` — this is the case on the current placeholder pinout |
| `UART BAUD <n>` | `OK UART BAUD <n>` | manual override; normally the host setting CDC line coding on the passthrough port is enough |
| `UART STATUS` | `OK UART BAUD <n>` | |
| `STATUS` | `OK POWER=<ON\|OFF> UART_BAUD=<n> UPTIME=<sec>` | |
| `SPI ...` / `I2C ...` | `ERR NOT_IMPLEMENTED` | reserved verb namespace, not functional yet |
| anything else / bad args | `ERR UNKNOWN_COMMAND` / `ERR BAD_ARG` | |

DUT UART baud rate itself is not set via a jig command in normal use — it
tracks whatever baud the host sets on the UART passthrough CDC port
(`SetLineCoding`), propagated live to the physical UART; `UART BAUD` is an
escape hatch, not the primary path.

See [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for full framing details (line
length limit, error tokens, the reserved `EVT` prefix for a possible future
async-event mode) and [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for how
this maps onto the firmware's module structure.
