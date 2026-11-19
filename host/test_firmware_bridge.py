"""Cross-language regression against production C protocol/motor (no hardware).
Build test/focus_simulator.exe first, then python host/test_firmware_bridge.py.
"""
import json
import pathlib
import queue
import subprocess
import threading
import unittest
import urllib.request
from focus_bridge import Focuser, Transport, AlpacaBridge, serve


class ProcessTransport(Transport):
    def __init__(self):
        executable = pathlib.Path(__file__).resolve().parent.parent / "test" / "focus_simulator.exe"
        self.process = subprocess.Popen([str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL)
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.process.stdout:
            self.lines.put(line.decode("ascii").strip())

    def send(self, data):
        self.process.stdin.write(data)
        self.process.stdin.flush()

    def read_line(self, timeout=2):
        return self.lines.get(timeout=timeout)

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=2)
        self.reader.join(timeout=2)
        self.process.stdout.close()


class ProductionFirmwareTests(unittest.TestCase):
    def setUp(self):
        self.focuser = Focuser(ProcessTransport())
        self.addCleanup(self.focuser.close)

    def test_actual_move_finish_and_halt_status(self):
        self.assertIn("YacaoFocus", self.focuser.ping())
        self.focuser.set_rate(300)
        self.assertEqual(self.focuser.status()["moving"], 0)
        self.focuser.goto(2)
        self.assertEqual(self.focuser.status()["moving"], 1)
        self.focuser.command("@TICK")
        self.assertEqual(self.focuser.position(), 1)
        self.focuser.command("@TICK")
        self.assertEqual(self.focuser.status(), {"pos": 2, "rate": 300, "volt": 12000, "moving": 0})
        self.focuser.goto(20)
        self.focuser.halt()
        self.assertEqual(self.focuser.status()["moving"], 0)
        self.assertEqual(self.focuser.status()["rate"], 300)

    def test_zero_target_stops_without_position_change(self):
        self.focuser.goto(10)
        self.focuser.goto(0)
        self.focuser.command("@TICK")
        self.assertEqual(self.focuser.status()["pos"], 0)
        self.assertEqual(self.focuser.status()["moving"], 0)

    def test_http_to_production_firmware_motion_completion(self):
        bridge = AlpacaBridge(self.focuser)
        server = serve(bridge, 0)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        def request(member, body=None):
            url = f"http://127.0.0.1:{server.alpaca_port}/api/v1/focuser/0/{member}"
            req = urllib.request.Request(url, data=body, method="PUT" if body is not None else "GET")
            with urllib.request.urlopen(req, timeout=3) as response:
                reply = json.loads(response.read())
            self.assertEqual(reply["ErrorNumber"], 0)
            return reply.get("Value")
        request("connected", b"Connected=true")
        request("move", b"Position=1")
        self.assertTrue(request("ismoving"))
        self.focuser.command("@TICK")
        self.assertFalse(request("ismoving"))
        self.assertEqual(request("position"), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
