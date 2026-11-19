#!/usr/bin/env python3
"""Offline boundary regressions: python host/test_focus_bridge.py.

The old POST/action=get tests asserted a private API, not Alpaca. These checks
use the published Alpaca GET/PUT routes and transaction/error envelopes.
"""
import concurrent.futures
import http.client
import json
import os
import socket
import sys
import threading
import time
import unittest
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from focus_bridge import (AlpacaBridge, Focuser, Fp1Error, SocketTransport, Transport,
                          ALPACA_DISCOVERY_REQUEST, serve)


class FakeDevice:
    """Wire fixture, not a hardware substitute. Motion and rate are independent."""
    def __init__(self):
        self.position = 1234
        self.rate = 250
        self.moving = False
        self.commands = []
        self.overrides = {}

    def handle(self, command):
        self.commands.append(command)
        if command in self.overrides:
            return self.overrides[command]
        if command == "PING":
            return "PONG YacaoFocus 1.001"
        if command == "VER?":
            return "VER 1.001"
        if command == "VOLT?":
            return "VOLT 12000"
        if command == "POS?":
            return f"POS {self.position}"
        if command == "STATUS?":
            return f"STATUS pos={self.position} rate={self.rate} volt=12000 moving={int(self.moving)}"
        if command.startswith("GOTO "):
            self.position = int(command.split()[1])
            self.moving = True
            return "OK"
        if command.startswith("MOVE "):
            self.position += int(command.split()[1])
            self.moving = True
            return "OK"
        if command.startswith("RATE "):
            self.rate = int(command.split()[1])
            return "OK"
        if command == "HALT":
            self.moving = False
            return "OK"
        return "ERR UNKNOWN"


class LoopbackTransport(Transport):
    def __init__(self, device):
        self.device = device
        self.pending = []
        self.closed = False

    def send(self, data):
        self.pending.append(self.device.handle(data.decode("ascii").strip()))

    def read_line(self, timeout=2):
        time.sleep(0.0005)  # Force overlap if the transaction lock is removed.
        return self.pending.pop(0)

    def close(self):
        self.closed = True


class FocuserTests(unittest.TestCase):
    def setUp(self):
        self.device = FakeDevice()
        self.transport = LoopbackTransport(self.device)
        self.focuser = Focuser(self.transport)
        self.addCleanup(self.focuser.close)

    def test_identity_and_numeric_values(self):
        self.assertEqual(self.focuser.ping(), "PONG YacaoFocus 1.001")
        self.assertEqual(self.focuser.version(), "1.001")
        self.assertEqual(self.focuser.position(), 1234)
        self.assertEqual(self.focuser.voltage_mv(), 12000)

    def test_rate_is_not_motion(self):
        self.focuser.set_rate(300)
        self.assertEqual(self.focuser.status()["moving"], 0)
        self.focuser.move_relative(1)
        self.assertEqual(self.focuser.status()["moving"], 1)
        self.focuser.halt()
        self.assertEqual(self.focuser.status()["rate"], 300)
        self.assertEqual(self.focuser.status()["moving"], 0)

    def test_error_reply_never_becomes_position_zero_or_move_success(self):
        for command, call in [("POS?", self.focuser.position), ("STATUS?", self.focuser.status),
                              ("GOTO 5", lambda: self.focuser.goto(5))]:
            self.device.overrides[command] = "ERR STATE"
            with self.assertRaises(Fp1Error):
                call()

    def test_malformed_status_rejected(self):
        for reply in ["STATUS", "STATUS pos=0", "STATUS pos=1 rate=2 volt=3 moving=2",
                      "STATUS pos=1 pos=2 rate=2 volt=3 moving=0", "STATUS pos=x rate=2 volt=3 moving=0"]:
            self.device.overrides["STATUS?"] = reply
            with self.assertRaises(Fp1Error):
                self.focuser.status()

    def test_malformed_position_rejected(self):
        for reply in ["OK", "POS 1.5", "POS 2147483648"]:
            self.device.overrides["POS?"] = reply
            with self.assertRaises(Fp1Error):
                self.focuser.position()

    def test_full_transaction_lock(self):
        def call(n):
            return self.focuser.position() if n % 2 else self.focuser.status()["pos"]
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            self.assertEqual(list(executor.map(call, range(120))), [1234] * 120)

    def test_single_line_command_boundary(self):
        with self.assertRaises(Fp1Error):
            self.focuser.command("PING\nGOTO 1")
        self.assertEqual(self.device.commands, [])

    def test_timeout_closes_session_preventing_late_reply_reuse(self):
        def timeout():
            raise TimeoutError("test timeout")
        self.transport.read_line = timeout
        with self.assertRaises(TimeoutError):
            self.focuser.position()
        self.assertTrue(self.transport.closed)
        with self.assertRaises(Fp1Error):
            self.focuser.position()
        self.assertEqual(self.device.commands, ["POS?"])

    def test_calibration_commands_are_explicit_and_bounded(self):
        self.assertEqual(self.device.commands, [])  # constructing client never sets zero
        for command in ("ZERO", "CALBEGIN", "MAXSET", "JOG -1", "JOG 100"):
            self.device.overrides[command] = "OK"
        self.focuser.confirm_zero()
        self.focuser.begin_calibration()
        self.focuser.jog(-1)
        self.focuser.jog(100)
        self.focuser.save_maximum()
        self.assertEqual(self.device.commands, ["ZERO", "CALBEGIN", "JOG -1", "JOG 100", "MAXSET"])
        before = list(self.device.commands)
        for invalid in (0, 101, -101, 1.5):
            with self.assertRaises(ValueError):
                self.focuser.jog(invalid)
        self.assertEqual(self.device.commands, before)
        self.device.overrides["MAXSET"] = "ERR STATE"
        with self.assertRaises(Fp1Error):
            self.focuser.save_maximum()

    def test_limits_parse_trust_state_without_guessing(self):
        for state, maximum in ((0, 1000), (1, 0), (2, 1000)):
            self.device.overrides["LIMIT?"] = f"LIMIT state={state} max={maximum}"
            self.assertEqual(self.focuser.limits(), {"state": state, "max": maximum})
        for reply in ("LIMIT max=1000", "LIMIT state=3 max=1", "LIMIT state=2 max=0",
                      "LIMIT state=1 state=2 max=1000", "LIMIT state=0 max=-1"):
            self.device.overrides["LIMIT?"] = reply
            with self.assertRaises(Fp1Error):
                self.focuser.limits()

    def test_tcp_buffer_keeps_coalesced_lines(self):
        client, peer = socket.socketpair()
        self.addCleanup(peer.close)
        transport = SocketTransport.__new__(SocketTransport)
        transport._socket, transport._buffer = client, b""
        self.addCleanup(transport.close)
        peer.sendall(b"POS 1\r\nSTATUS pos=1 rate=1 volt=1 moving=0\n")
        self.assertEqual(transport.read_line(), "POS 1")
        self.assertEqual(transport.read_line(), "STATUS pos=1 rate=1 volt=1 moving=0")


class AlpacaTests(unittest.TestCase):
    def setUp(self):
        self.device = FakeDevice()
        self.focuser = Focuser(LoopbackTransport(self.device))
        self.bridge = AlpacaBridge(self.focuser, "TestFocus", device_number=3)
        self.server = serve(self.bridge, 0, discovery=True, discovery_port=0)
        self.base = "/api/v1/focuser/3/"
        self.addCleanup(self.focuser.close)
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.addCleanup(self.server.discovery.stop)

    def request(self, method, path, params=None, body=None, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.alpaca_port, timeout=3)
        try:
            if params:
                if method == "PUT":
                    body = urllib.parse.urlencode(params)
                    headers = {"Content-Type": "application/x-www-form-urlencoded"}
                else:
                    path += "?" + urllib.parse.urlencode(params)
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            payload = json.loads(response.read())
            self.assertIn("ClientTransactionID", payload)
            self.assertGreater(payload["ServerTransactionID"], 0)
            self.assertIn("ErrorMessage", payload)
            return response.status, payload
        finally:
            connection.close()

    def connect(self):
        status, payload = self.request("PUT", self.base + "connected", {"Connected": "true"})
        self.assertEqual((status, payload["ErrorNumber"]), (200, 0))

    def test_connection_and_not_connected_error(self):
        self.assertFalse(self.request("GET", self.base + "connected")[1]["Value"])
        self.assertEqual(self.request("GET", self.base + "position")[1]["ErrorNumber"], 0x407)
        self.connect()
        self.assertEqual(self.request("GET", self.base + "position")[1]["Value"], 1234)
        self.request("PUT", self.base + "connected", {"Connected": "false"})
        self.assertFalse(self.request("GET", self.base + "connected")[1]["Value"])

    def test_standard_common_and_focuser_properties(self):
        for member, expected in {"interfaceversion": 2, "maxstep": 60000, "maxincrement": 60000,
                                 "absolute": True, "tempcompavailable": False, "tempcomp": False,
                                 "name": "TestFocus", "supportedactions": []}.items():
            status, payload = self.request("GET", self.base + member)
            self.assertEqual((status, payload["ErrorNumber"], payload["Value"]), (200, 0, expected))

    def test_put_body_case_insensitive_parameters_transactions_and_motion(self):
        self.connect()
        status, payload = self.request("PUT", self.base + "move", {"pOsItIoN": "200", "cLiEnTtRaNsAcTiOnId": "17"})
        self.assertEqual((status, payload["ClientTransactionID"], payload["ErrorNumber"]), (200, 17, 0))
        self.assertNotIn("Value", payload)
        self.assertEqual(self.device.position, 200)
        self.assertTrue(self.request("GET", self.base + "ismoving")[1]["Value"])
        self.request("PUT", self.base + "halt")
        self.assertFalse(self.request("GET", self.base + "ismoving")[1]["Value"])
        self.assertEqual(self.device.rate, 250)

    def test_unknown_physical_capabilities_not_fabricated(self):
        self.connect()
        for member in ("stepsize", "temperature"):
            status, payload = self.request("GET", self.base + member)
            self.assertEqual((status, payload["ErrorNumber"]), (200, 0x400))
            self.assertNotIn("Value", payload)

    def test_range_validation_and_no_motion_side_effect(self):
        self.connect()
        for value in ("-1", "60001"):
            self.assertEqual(self.request("PUT", self.base + "move", {"Position": value})[1]["ErrorNumber"], 0x401)
        for value in ("1.2", "garbage", "2147483648"):
            self.assertEqual(self.request("PUT", self.base + "move", {"Position": value})[0], 400)
        self.assertEqual(self.request("PUT", self.base + "move")[0], 400)
        self.assertFalse(any(c.startswith("GOTO") for c in self.device.commands))

    def test_fp1_error_projects_error_not_http_success(self):
        self.connect()
        self.device.overrides["GOTO 5"] = "ERR STATE"
        self.assertEqual(self.request("PUT", self.base + "move", {"Position": "5"})[1]["ErrorNumber"], 0x500)
        self.device.overrides["STATUS?"] = "ERR STATE"
        payload = self.request("GET", self.base + "ismoving")[1]
        self.assertEqual(payload["ErrorNumber"], 0x500)
        self.assertNotIn("Value", payload)

    def test_routes_reject_wrong_device_and_private_api(self):
        for path in ("/api/v1/focuser/0/position", self.base + "properties", self.base,
                     "/api/v1/focuser/3/position/extra", "/api/v1/Focuser/3/position"):
            self.assertEqual(self.request("GET", path)[0], 404)
        self.assertEqual(self.request("GET", self.base + "move")[0], 405)
        self.assertEqual(self.request("POST", self.base + "move")[0], 405)

    def test_duplicate_parameter_and_invalid_transaction_rejected(self):
        self.assertEqual(self.request("PUT", self.base + "connected?Connected=true", body="connected=false",
                                     headers={"Content-Type": "application/x-www-form-urlencoded"})[0], 400)
        for value in ("-1", "abc", "4294967296"):
            self.assertEqual(self.request("GET", self.base + "connected", {"ClientTransactionID": value})[0], 400)
        self.assertEqual(self.request("GET", self.base + "connected", {"ClientID": "bad"})[0], 400)

    def test_unique_server_transaction_ids(self):
        def read(n):
            return self.request("GET", self.base + "connected", {"ClientTransactionID": str(n)})[1]
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
            replies = list(executor.map(read, range(20)))
        self.assertEqual([r["ClientTransactionID"] for r in replies], list(range(20)))
        self.assertEqual(len({r["ServerTransactionID"] for r in replies}), 20)

    def test_management_and_discovery_actual_ephemeral_port(self):
        for path, expected in [("/management/apiversions", [1])]:
            self.assertEqual(self.request("GET", path)[1]["Value"], expected)
        devices = self.request("GET", "/management/v1/configureddevices")[1]["Value"]
        self.assertEqual(devices[0]["DeviceNumber"], 3)
        self.assertEqual(devices[0]["DeviceName"], "TestFocus")
        self.assertTrue(devices[0]["UniqueID"])
        self.assertIn("ManufacturerVersion", self.request("GET", "/management/v1/description")[1]["Value"])
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
            client.settimeout(1)
            client.sendto(ALPACA_DISCOVERY_REQUEST, ("127.0.0.1", self.server.discovery.port))
            payload = json.loads(client.recv(4096))
            self.assertEqual(payload, {"AlpacaPort": self.server.alpaca_port})
            client.sendto(b"garbage" + ALPACA_DISCOVERY_REQUEST, ("127.0.0.1", self.server.discovery.port))
            with self.assertRaises(socket.timeout):
                client.recv(4096)


if __name__ == "__main__":
    unittest.main(verbosity=2)
