#!/usr/bin/env python3
"""Host-side control CLI for the glasnost debug jig. See docs/PROTOCOL.md."""

import argparse
import os
import select
import sys
import time
from collections import defaultdict

import serial
from serial.tools import list_ports

DEFAULT_PROBE_TIMEOUT = 0.3
DEFAULT_RESPONSE_TIMEOUT = 2.0
DEFAULT_PROBE_BAUD = 115200
DEFAULT_MONITOR_BAUD = 115200

EXIT_ERR_RESPONSE = 1
EXIT_DISCOVERY = 2
EXIT_COMM = 3


class JigError(Exception):
    exit_code = EXIT_COMM


class NoJigFoundError(JigError):
    exit_code = EXIT_DISCOVERY


class AmbiguousJigError(JigError):
    exit_code = EXIT_DISCOVERY


class JigTimeoutError(JigError):
    exit_code = EXIT_COMM


class JigCommError(JigError):
    exit_code = EXIT_COMM


class JigProtocolError(JigError):
    exit_code = EXIT_ERR_RESPONSE

    def __init__(self, reason):
        super().__init__(f"ERR {reason}")
        self.reason = reason


class JigInfo:
    def __init__(self, key, control, passthrough, extra):
        self.key = key
        self.control = control
        self.passthrough = passthrough
        self.extra = extra

    def label(self):
        control_path = self.control.device
        passthrough_path = self.passthrough.device if self.passthrough else "?"
        sn = self.control.serial_number or "-"
        return f"control={control_path} passthrough={passthrough_path} serial={sn}"


def request(ser, verb_line, timeout):
    ser.reset_input_buffer()
    ser.write((verb_line + "\n").encode("ascii"))
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise JigTimeoutError(
                f"no response to {verb_line!r} within {timeout:.2f}s"
            )
        ser.timeout = remaining
        raw = ser.readline()
        if not raw:
            raise JigTimeoutError(
                f"no response to {verb_line!r} within {timeout:.2f}s"
            )
        line = raw.decode("ascii", errors="replace").rstrip("\r\n")
        if not line:
            continue
        if line.startswith("OK") or line.startswith("ERR"):
            return line
        # PROTOCOL.md reserves "EVT " for a future unsolicited-event mode and
        # says a line not matching the request we just sent is ignorable, not
        # an error -- keep waiting for the real OK/ERR reply. Anything else
        # is genuinely malformed and reported as such rather than silently
        # eaten until the timeout fires.
        if not line.startswith("EVT"):
            raise JigCommError(f"unexpected response line: {line!r}")


def parse_response(line):
    if line == "OK":
        return ""
    if line.startswith("OK "):
        return line[3:]
    if line.startswith("ERR "):
        raise JigProtocolError(line[4:])
    if line == "ERR":
        raise JigProtocolError("UNKNOWN")
    raise JigCommError(f"unexpected response line: {line!r}")


def query(ser, verb_line, timeout):
    return parse_response(request(ser, verb_line, timeout))


def group_key(port_info):
    if port_info.serial_number:
        return ("sn", port_info.serial_number)
    if port_info.location:
        # Both CDC interfaces of one physical device share the same USB
        # topology path and differ only in the trailing ":<cfg>.<iface>"
        # suffix, e.g. "3-1.4:1.0" vs "3-1.4:1.2" -- strip it to pair them.
        return ("loc", port_info.location.split(":")[0])
    return ("dev", port_info.device)


def matches_needle(port_info, needle):
    needle = needle.lower()
    fields = (
        port_info.device,
        port_info.serial_number,
        port_info.location,
        port_info.hwid,
        port_info.manufacturer,
        port_info.product,
    )
    return any(f and needle in f.lower() for f in fields)


def list_candidate_ports(vid=None, pid=None):
    ports = list(list_ports.comports())
    if vid is not None:
        ports = [p for p in ports if p.vid == vid]
    if pid is not None:
        ports = [p for p in ports if p.pid == pid]
    return ports


def probe_port(device_path, probe_baud, probe_timeout):
    try:
        with serial.Serial(device_path, baudrate=probe_baud, timeout=probe_timeout) as ser:
            return query(ser, "PING", probe_timeout) == "PONG"
    except (JigError, serial.SerialException, OSError):
        return False


def discover_jigs(vid=None, pid=None, probe_baud=DEFAULT_PROBE_BAUD,
                   probe_timeout=DEFAULT_PROBE_TIMEOUT):
    candidates = list_candidate_ports(vid, pid)
    groups = defaultdict(list)
    for p in candidates:
        groups[group_key(p)].append(p)

    jigs = []
    for key, members in groups.items():
        control = None
        others = []
        for p in members:
            if probe_port(p.device, probe_baud, probe_timeout):
                control = p
            else:
                others.append(p)
        if control is not None:
            passthrough = others[0] if len(others) == 1 else None
            extra = others if len(others) != 1 else []
            jigs.append(JigInfo(key, control, passthrough, extra))
    return jigs


def find_sibling_port(control_device_path, vid=None, pid=None):
    candidates = list_candidate_ports(vid, pid)
    target = next((p for p in candidates if p.device == control_device_path), None)
    if target is None:
        return None
    key = group_key(target)
    siblings = [p for p in candidates
                if p.device != control_device_path and group_key(p) == key]
    return siblings[0].device if len(siblings) == 1 else None


def resolve_control_port(args):
    if args.port:
        return args.port, None

    jigs = discover_jigs(vid=args.vid, pid=args.pid,
                          probe_timeout=args.probe_timeout)
    if args.device:
        jigs = [j for j in jigs
                if matches_needle(j.control, args.device)
                or (j.passthrough and matches_needle(j.passthrough, args.device))]

    if not jigs:
        raise NoJigFoundError(
            "no jig control port found (probed all candidate serial ports "
            "with PING, none replied OK PONG). Use --port to force one."
        )
    if len(jigs) > 1:
        listing = "\n".join(f"  - {j.label()}" for j in jigs)
        raise AmbiguousJigError(
            f"multiple jigs found, pass --device to disambiguate:\n{listing}"
        )

    jig = jigs[0]
    passthrough = jig.passthrough.device if jig.passthrough else None
    return jig.control.device, passthrough


def resolve_uart_port(args, control_path):
    if args.uart_port:
        return args.uart_port
    sibling = find_sibling_port(control_path, vid=args.vid, pid=args.pid)
    if sibling is None:
        raise JigCommError(
            "could not identify the UART passthrough port sibling to the "
            f"control port {control_path!r}; pass --uart-port explicitly"
        )
    return sibling


def open_control(args):
    control_path, _ = resolve_control_port(args)
    try:
        return serial.Serial(control_path, baudrate=DEFAULT_PROBE_BAUD,
                              timeout=args.timeout)
    except serial.SerialException as exc:
        raise JigCommError(f"could not open control port {control_path!r}: {exc}")


def cmd_ping(args):
    with open_control(args) as ser:
        data = query(ser, "PING", args.timeout)
        print(data)


def cmd_version(args):
    with open_control(args) as ser:
        data = query(ser, "VERSION", args.timeout)
        print(data)


def cmd_power_on(args):
    with open_control(args) as ser:
        print(query(ser, "POWER ON", args.timeout))


def cmd_power_off(args):
    with open_control(args) as ser:
        print(query(ser, "POWER OFF", args.timeout))


def cmd_power_cycle(args):
    verb = "POWER CYCLE"
    wait_ms = args.ms if args.ms is not None else 1000
    if args.ms is not None:
        verb += f" {args.ms}"
    timeout = args.timeout + wait_ms / 1000.0
    with open_control(args) as ser:
        print(query(ser, verb, timeout))


def cmd_power_status(args):
    with open_control(args) as ser:
        print(query(ser, "POWER STATUS", args.timeout))


def cmd_reset(args):
    verb = "RESET"
    pulse_ms = args.ms if args.ms is not None else 200
    if args.ms is not None:
        verb += f" {args.ms}"
    timeout = args.timeout + pulse_ms / 1000.0
    with open_control(args) as ser:
        print(query(ser, verb, timeout))


def cmd_uart_baud(args):
    with open_control(args) as ser:
        print(query(ser, f"UART BAUD {args.baud}", args.timeout))


def cmd_uart_status(args):
    with open_control(args) as ser:
        print(query(ser, "UART STATUS", args.timeout))


def cmd_status(args):
    with open_control(args) as ser:
        print(query(ser, "STATUS", args.timeout))


def cmd_list(args):
    jigs = discover_jigs(vid=args.vid, pid=args.pid, probe_timeout=args.probe_timeout)
    if not jigs:
        print("no jigs found", file=sys.stderr)
        return
    for j in jigs:
        print(j.label())


def cmd_monitor(args):
    control_path, passthrough_hint = resolve_control_port(args)
    uart_path = args.uart_port or passthrough_hint or resolve_uart_port(args, control_path)

    if not sys.stdin.isatty():
        raise JigCommError("monitor requires an interactive terminal on stdin")

    print(f"jigctl monitor: {uart_path} @ {args.baud} baud "
          f"(control port: {control_path}). Ctrl-] or Ctrl-C to exit.",
          file=sys.stderr)

    try:
        ser = serial.Serial(uart_path, baudrate=args.baud, timeout=0)
    except serial.SerialException as exc:
        raise JigCommError(f"could not open UART passthrough port {uart_path!r}: {exc}")

    import termios
    import tty

    fd = sys.stdin.fileno()
    old_settings = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        while True:
            rlist, _, _ = select.select([sys.stdin, ser], [], [])
            if ser in rlist:
                data = ser.read(ser.in_waiting or 1)
                if data:
                    os.write(sys.stdout.fileno(), data)
            if sys.stdin in rlist:
                ch = os.read(fd, 1)
                if ch in (b"\x1d", b"\x03"):
                    break
                ser.write(ch)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
        ser.close()


def vid_pid_type(value):
    return int(value, 0)


def build_parser():
    parser = argparse.ArgumentParser(
        prog="jigctl", description="Control tool for the glasnost debug jig."
    )
    parser.add_argument("--port", help="control port device path; skips auto-discovery")
    parser.add_argument("--device",
                         help="USB serial number / by-id path / location substring "
                              "to disambiguate when multiple jigs are attached")
    parser.add_argument("--vid", type=vid_pid_type, default=None,
                         help="filter candidate ports by USB VID (e.g. 0x303A)")
    parser.add_argument("--pid", type=vid_pid_type, default=None,
                         help="filter candidate ports by USB PID")
    parser.add_argument("--probe-timeout", type=float, default=DEFAULT_PROBE_TIMEOUT,
                         help="per-port PING probe timeout in seconds "
                              f"(default {DEFAULT_PROBE_TIMEOUT})")
    parser.add_argument("--timeout", type=float, default=DEFAULT_RESPONSE_TIMEOUT,
                         help="response timeout in seconds for issued commands "
                              f"(default {DEFAULT_RESPONSE_TIMEOUT})")

    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("ping").set_defaults(func=cmd_ping)
    sub.add_parser("version").set_defaults(func=cmd_version)

    p_power = sub.add_parser("power")
    power_sub = p_power.add_subparsers(dest="power_action", required=True)
    power_sub.add_parser("on").set_defaults(func=cmd_power_on)
    power_sub.add_parser("off").set_defaults(func=cmd_power_off)
    p_cycle = power_sub.add_parser("cycle")
    p_cycle.add_argument("--ms", type=int, default=None,
                          help="off duration before re-enabling (jig default 1000)")
    p_cycle.set_defaults(func=cmd_power_cycle)
    power_sub.add_parser("status").set_defaults(func=cmd_power_status)

    p_reset = sub.add_parser("reset")
    p_reset.add_argument("--ms", type=int, default=None,
                          help="reset pulse width (jig default 200)")
    p_reset.set_defaults(func=cmd_reset)

    p_uart_baud = sub.add_parser("uart-baud")
    p_uart_baud.add_argument("baud", type=int)
    p_uart_baud.set_defaults(func=cmd_uart_baud)

    sub.add_parser("uart-status").set_defaults(func=cmd_uart_status)
    sub.add_parser("status").set_defaults(func=cmd_status)
    sub.add_parser("list").set_defaults(func=cmd_list)

    p_monitor = sub.add_parser(
        "monitor", help="bridge the DUT UART passthrough port to stdin/stdout"
    )
    p_monitor.add_argument("--baud", type=int, default=DEFAULT_MONITOR_BAUD)
    p_monitor.add_argument("--uart-port",
                            help="UART passthrough port device path; skips sibling lookup")
    p_monitor.set_defaults(func=cmd_monitor)

    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        args.func(args)
    except JigError as exc:
        print(f"jigctl: {exc}", file=sys.stderr)
        sys.exit(exc.exit_code)
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
