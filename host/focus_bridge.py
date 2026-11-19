#!/usr/bin/env python3
"""FP1 to Alpaca IFocuserV2 bridge (offline contract-tested, not client-certified).

python host/focus_bridge.py --port COM7 --alpaca-port 8090 --discovery
Use --tcp HOST:PORT instead of --port for a network transport.
Positions are steps from the current power-on origin, not homed coordinates.
"""
from __future__ import annotations

import argparse
import http.server
import json
import re
import socket
import threading
import time
import urllib.parse

DEFAULT_BAUD = 115200
DEFAULT_ALPACA_PORT = 8090
FP1_TIMEOUT = 2.0
ALPACA_DISCOVERY_PORT = 3220
ALPACA_DISCOVERY_REQUEST = b"alpacadiscovery1"


class Fp1Error(Exception):
    pass


def _integer(text: str, minimum: int = -(2**31), maximum: int = 2**31 - 1) -> int:
    if not re.fullmatch(r"[+-]?[0-9]+", text):
        raise ValueError("decimal integer required")
    value = int(text)
    if not minimum <= value <= maximum:
        raise ValueError("integer out of range")
    return value


class Transport:
    def send(self, data: bytes) -> None:
        raise NotImplementedError

    def read_line(self, timeout: float = FP1_TIMEOUT) -> str:
        raise NotImplementedError

    def close(self) -> None:
        pass


class _BufferedTransport(Transport):
    """Preserve extra received bytes and bound both frame length and total time."""
    def read_line(self, timeout: float = FP1_TIMEOUT) -> str:
        deadline = time.monotonic() + timeout
        while b"\n" not in self._buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("no complete reply from focuser")
            self._buffer += self._read(remaining)
            if len(self._buffer.split(b"\n", 1)[0]) > 255:
                raise Fp1Error("overlong FP1 reply")
        line, self._buffer = self._buffer.split(b"\n", 1)
        try:
            return line.decode("ascii").strip()
        except UnicodeError as error:
            raise Fp1Error("non-ASCII FP1 reply") from error


class SerialTransport(_BufferedTransport):
    def __init__(self, port: str, baud: int = DEFAULT_BAUD) -> None:
        import serial
        self._serial = serial.Serial(port, baud, timeout=0.1, write_timeout=FP1_TIMEOUT)
        self._buffer = b""

    def send(self, data: bytes) -> None:
        self._serial.write(data)
        self._serial.flush()

    def _read(self, timeout: float) -> bytes:
        self._serial.timeout = min(timeout, 0.1)
        return self._serial.read(128)

    def close(self) -> None:
        self._serial.close()


class SocketTransport(_BufferedTransport):
    def __init__(self, host: str, port: int) -> None:
        self._socket = socket.create_connection((host, port), timeout=FP1_TIMEOUT)
        self._buffer = b""

    def send(self, data: bytes) -> None:
        self._socket.sendall(data)

    def _read(self, timeout: float) -> bytes:
        self._socket.settimeout(timeout)
        data = self._socket.recv(128)
        if not data:
            raise ConnectionError("focuser closed connection")
        return data

    def close(self) -> None:
        self._socket.close()


class Focuser:
    def __init__(self, transport: Transport) -> None:
        self._transport = transport
        self._lock = threading.RLock()
        self._closed = False

    def close(self) -> None:
        with self._lock:
            if not self._closed:
                self._closed = True
                self._transport.close()

    @property
    def is_open(self) -> bool:
        with self._lock:
            return not self._closed

    def command(self, text: str) -> str:
        with self._lock:
            if self._closed:
                raise Fp1Error("transport closed; restart bridge to reconnect")
            if not text or "\n" in text or "\r" in text:
                raise Fp1Error("one command required")
            try:
                self._transport.send(text.encode("ascii") + b"\n")
                reply = self._transport.read_line()
            except (OSError, Fp1Error):
                self.close()  # A late answer must never become the next command's reply.
                raise
            if reply == "ERR" or reply.startswith("ERR "):
                raise Fp1Error(reply)
            return reply

    def ping(self) -> str:
        reply = self.command("PING")
        if not reply.startswith("PONG "):
            raise Fp1Error("unexpected PING reply")
        return reply

    def _value(self, command: str, prefix: str) -> str:
        reply = self.command(command)
        if not reply.startswith(prefix + " "):
            raise Fp1Error("unexpected " + command + " reply")
        return reply[len(prefix) + 1:]

    def version(self) -> str:
        return self._value("VER?", "VER")

    def position(self) -> int:
        try:
            return _integer(self._value("POS?", "POS"))
        except ValueError as error:
            raise Fp1Error("invalid POS reply") from error

    def status(self) -> dict:
        try:
            fields = {}
            for part in self._value("STATUS?", "STATUS").split():
                key, text = part.split("=")
                if key in fields:
                    raise ValueError("duplicate field")
                fields[key] = _integer(text)
            if set(fields) != {"pos", "rate", "volt", "moving"} or fields["moving"] not in (0, 1):
                raise ValueError("missing/invalid status fields")
            return fields
        except ValueError as error:
            raise Fp1Error("invalid STATUS reply") from error

    def _ok(self, command: str) -> str:
        if self.command(command) != "OK":
            raise Fp1Error("unexpected " + command + " reply")
        return "OK"

    def move_relative(self, steps: int) -> str:
        return self._ok(f"MOVE {_integer(str(steps))}")

    def goto(self, steps: int) -> str:
        return self._ok(f"GOTO {_integer(str(steps))}")

    def set_rate(self, steps_per_second: int) -> str:
        return self._ok(f"RATE {_integer(str(steps_per_second), 0)}")

    def halt(self) -> str:
        return self._ok("HALT")

    def confirm_zero(self) -> str:
        return self._ok("ZERO")

    def begin_calibration(self) -> str:
        return self._ok("CALBEGIN")

    def save_maximum(self) -> str:
        return self._ok("MAXSET")

    def jog(self, steps: int) -> str:
        value = _integer(str(steps), -100, 100)
        if value == 0:
            raise ValueError("nonzero jog required")
        return self._ok(f"JOG {value}")

    def limits(self) -> dict:
        try:
            fields = {}
            for part in self._value("LIMIT?", "LIMIT").split():
                key, value = part.split("=")
                if key in fields:
                    raise ValueError("duplicate limit field")
                fields[key] = _integer(value, 0)
            if set(fields) != {"state", "max"} or fields["state"] not in (0, 1, 2):
                raise ValueError("invalid calibration state")
            if fields["state"] == 2 and fields["max"] == 0:
                raise ValueError("ready without a travel limit")
            return fields
        except ValueError as error:
            raise Fp1Error("invalid LIMIT reply") from error

    def voltage_mv(self) -> int:
        try:
            return _integer(self._value("VOLT?", "VOLT"))
        except ValueError as error:
            raise Fp1Error("invalid VOLT reply") from error


class AlpacaError(Exception):
    def __init__(self, number: int, message: str):
        super().__init__(message)
        self.number = number


class AlpacaBridge:
    def __init__(self, focuser: Focuser, name: str = "REAFocus", device_number: int = 0,
                 max_step: int = 60000) -> None:
        if device_number < 0 or not 1 <= max_step <= 2**31 - 1:
            raise ValueError("invalid device number or maximum position")
        self._focuser = focuser
        self._name = name
        self._device_number = device_number
        self._max_step = max_step
        self._connected = False
        self._lock = threading.RLock()
        self._transaction = 0

    def envelope(self, transaction: int, value=None, error: AlpacaError | None = None,
                 has_value: bool = False) -> dict:
        with self._lock:
            self._transaction = self._transaction % (2**32 - 1) + 1
            payload = {"ClientTransactionID": transaction, "ServerTransactionID": self._transaction,
                       "ErrorNumber": error.number if error else 0, "ErrorMessage": str(error) if error else ""}
            if has_value and error is None:
                payload["Value"] = value
            return payload

    def configured_devices(self) -> list:
        return [{"DeviceName": self._name, "DeviceType": "Focuser", "DeviceNumber": self._device_number,
                 "UniqueID": f"rea-fp1-focuser-{self._device_number}"}]

    def operation(self, method: str, member: str, parameters: dict):
        with self._lock:
            connected = self._connected and self._focuser.is_open
            common = {"connected": connected, "name": self._name,
                      "description": "Reconstructed FP1 focuser (power-on step origin)",
                      "driverinfo": "REA FP1 Alpaca bridge; no hardware/client certification",
                      "driverversion": "1.1", "interfaceversion": 2, "supportedactions": [],
                      "absolute": True, "maxstep": self._max_step, "maxincrement": self._max_step,
                      "tempcompavailable": False, "tempcomp": False}
            readable = set(common) | {"position", "ismoving", "stepsize", "temperature"}
            writable = {"connected", "move", "halt", "tempcomp", "action", "commandblind", "commandbool", "commandstring"}
            if member not in readable | writable:
                raise LookupError("unknown member")
            if method == "GET" and member not in readable or method == "PUT" and member not in writable:
                raise PermissionError("method not supported for member")
            if method == "GET" and member in common:
                return common[member]
            if method == "PUT" and member == "connected":
                text = parameters.get("connected", "").lower()
                if text not in ("true", "false"):
                    raise ValueError("Connected must be true or false")
                if text == "true":
                    self._focuser.ping()
                self._connected = text == "true"
                return None
            if not connected:
                raise AlpacaError(0x407, "focuser not connected")
            if member == "position":
                return self._focuser.position()
            if member == "ismoving":
                return bool(self._focuser.status()["moving"])
            if member == "move":
                if "position" not in parameters:
                    raise ValueError("Position is required")
                target = _integer(parameters["position"])
                if not 0 <= target <= self._max_step:
                    raise AlpacaError(0x401, "Position is outside 0..MaxStep")
                self._focuser.goto(target)
                return None
            if member == "halt":
                self._focuser.halt()
                return None
            if member == "action":
                raise AlpacaError(0x40C, "no supported actions")
            # Physical StepSize / Temperature are unknown, not guessed.
            raise AlpacaError(0x400, member + " is not implemented")


def _make_handler(bridge: AlpacaBridge):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args) -> None:
            pass

        def _reply(self, payload, status=200):
            body = json.dumps(payload).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _handle(self, method):
            transaction = 0
            try:
                parsed = urllib.parse.urlsplit(self.path)
                pairs = urllib.parse.parse_qsl(parsed.query, keep_blank_values=True)
                if self.headers.get("Transfer-Encoding"):
                    raise ValueError("chunked request bodies are unsupported")
                length = _integer(self.headers.get("Content-Length", "0"), 0, 4096)
                if method == "PUT" and length:
                    if self.headers.get_content_type() != "application/x-www-form-urlencoded":
                        raise ValueError("form URL encoded body required")
                    self.connection.settimeout(FP1_TIMEOUT)
                    body = self.rfile.read(length)
                    if len(body) != length:
                        raise ValueError("incomplete request body")
                    pairs += urllib.parse.parse_qsl(body.decode("utf-8"), keep_blank_values=True)
                elif length:
                    raise ValueError("unexpected request body")
                params = {}
                for key, value in pairs:
                    key = key.lower()
                    if key in params:
                        raise ValueError("duplicate parameter: " + key)
                    params[key] = value
                transaction = _integer(params.get("clienttransactionid", "0"), 0, 2**32 - 1)
                if "clientid" in params:
                    _integer(params["clientid"], 0, 2**32 - 1)
                path = parsed.path
                management = {"/management/apiversions": [1],
                              "/management/v1/description": {"ServerName": "REA focus bridge", "Manufacturer": "REA",
                                 "ManufacturerVersion": "1.1", "Location": "local"},
                              "/management/v1/configureddevices": bridge.configured_devices()}
                if path in management:
                    if method != "GET":
                        raise PermissionError("management is read-only")
                    self._reply(bridge.envelope(transaction, management[path], has_value=True))
                    return
                prefix = f"/api/v1/focuser/{bridge._device_number}/"
                if not path.startswith(prefix) or "/" in path[len(prefix):]:
                    raise LookupError("unknown device or endpoint")
                value = bridge.operation(method, path[len(prefix):], params)
                self._reply(bridge.envelope(transaction, value, has_value=method == "GET"))
            except LookupError as error:
                self._reply(bridge.envelope(transaction, error=AlpacaError(0x400, str(error))), 404)
            except PermissionError as error:
                self._reply(bridge.envelope(transaction, error=AlpacaError(0x400, str(error))), 405)
            except (ValueError, UnicodeError) as error:
                self.close_connection = True
                self._reply(bridge.envelope(transaction, error=AlpacaError(0x401, str(error))), 400)
            except AlpacaError as error:
                self._reply(bridge.envelope(transaction, error=error))
            except (Fp1Error, OSError) as error:
                self._reply(bridge.envelope(transaction, error=AlpacaError(0x500, str(error))))

        def do_GET(self):
            self._handle("GET")

        def do_PUT(self):
            self._handle("PUT")

        def do_POST(self):
            self.close_connection = True
            self._reply(bridge.envelope(0, error=AlpacaError(0x400, "use GET or PUT")), 405)

    return Handler


class AlpacaDiscovery:
    """Alpaca UDP discovery returns only the HTTP AlpacaPort, not management JSON."""
    def __init__(self, bridge: AlpacaBridge, alpaca_port: int, port: int = ALPACA_DISCOVERY_PORT,
                 host: str = "0.0.0.0") -> None:
        self._alpaca_port = alpaca_port
        self._port = port
        self._host = host
        self._socket = None
        self._thread = None
        self._stop = threading.Event()

    @property
    def port(self):
        return self._socket.getsockname()[1] if self._socket else self._port

    def start(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.bind((self._host, self._port))
            sock.settimeout(0.1)
        except BaseException:
            sock.close()
            raise
        self._socket = sock
        self._stop.clear()
        self._thread = threading.Thread(target=self._listen, daemon=True)
        self._thread.start()

    def _listen(self):
        payload = json.dumps({"AlpacaPort": self._alpaca_port}).encode("ascii")
        while not self._stop.is_set():
            try:
                data, address = self._socket.recvfrom(1024)
                if data == ALPACA_DISCOVERY_REQUEST:
                    self._socket.sendto(payload, address)
            except socket.timeout:
                continue
            except OSError:
                return

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=1)
        if self._socket:
            self._socket.close()
            self._socket = None


def serve(bridge: AlpacaBridge, port: int, discovery: bool = False,
          discovery_port: int = ALPACA_DISCOVERY_PORT, host: str = "127.0.0.1"):
    server = http.server.ThreadingHTTPServer((host, port), _make_handler(bridge))
    server.alpaca_port = server.server_address[1]
    if discovery:
        responder = AlpacaDiscovery(bridge, server.alpaca_port, discovery_port, host)
        try:
            responder.start()
        except BaseException:
            server.server_close()
            raise
        server.discovery = responder
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    transport_group = parser.add_mutually_exclusive_group(required=True)
    transport_group.add_argument("--port", help="explicit serial port")
    transport_group.add_argument("--tcp", metavar="HOST:PORT")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--alpaca-port", type=int, default=DEFAULT_ALPACA_PORT)
    parser.add_argument("--host", default="127.0.0.1", help="bind address; use 0.0.0.0 only on a trusted network")
    parser.add_argument("--name", default="REAFocus")
    parser.add_argument("--max-step", type=int, default=60000)
    parser.add_argument("--discovery", action="store_true")
    parser.add_argument("--discovery-port", type=int, default=ALPACA_DISCOVERY_PORT)
    args = parser.parse_args()
    if args.tcp:
        host, port = args.tcp.rsplit(":", 1)
        transport = SocketTransport(host, int(port))
    else:
        transport = SerialTransport(args.port, args.baud)
    focuser = Focuser(transport)
    server = None
    try:
        focuser.ping()
        bridge = AlpacaBridge(focuser, args.name, max_step=args.max_step)
        server = serve(bridge, args.alpaca_port, args.discovery, args.discovery_port, args.host)
        print(f"Alpaca focuser HTTP port {server.alpaca_port}")
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        return 0
    finally:
        if server:
            if hasattr(server, "discovery"):
                server.discovery.stop()
            server.shutdown()
            server.server_close()
        focuser.close()


if __name__ == "__main__":
    raise SystemExit(main())
