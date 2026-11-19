using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using ASCOM.DeviceInterface;

namespace REA.Focus.Test
{
    // Entirely offline: no COM activation, registry access or serial enumeration.
    public static class Program
    {
        private static int checks;
        private static void Check(bool ok, string name)
        {
            if (!ok) throw new Exception("FAIL " + name);
            checks++; Console.WriteLine("ok " + name);
        }
        private static void Throws<T>(Action action, string name) where T : Exception
        {
            try { action(); } catch (T) { Check(true, name); return; }
            throw new Exception("FAIL expected " + typeof(T).Name + ": " + name);
        }
        private static REAFocusDriver Driver(FakeDevice device)
        {
            return new REAFocusDriver(new Dictionary<string, string> {
                { "TcpEndpoint", "127.0.0.1:" + device.Port }, { "MaxStep", "60000" }
            });
        }
        public static int Main()
        {
            try
            {
                using (FakeDevice device = new FakeDevice())
                using (REAFocusDriver driver = Driver(device))
                {
                    Check(driver is IFocuserV2 && driver is IFocuserV3 && driver is IFocuserV4, "managed V2/V3/V4 bindings (not COM registration)");
                    Check(driver.InterfaceVersion == 4, "advertises V4");
                    Check(driver.Absolute && driver.MaxStep == 60000 && driver.MaxIncrement == 60000, "step-coordinate capabilities");
                    Throws<ASCOM.PropertyNotImplementedException>(() => { double x = driver.StepSize; }, "unknown physical step size");
                    Throws<ASCOM.NotConnectedException>(() => { int x = driver.Position; }, "disconnected read");
                    driver.Connected = true; Check(driver.Connected, "handshake connects");
                    Check(driver.Position == 1234 && !driver.IsMoving, "idle position/state");
                    Throws<ASCOM.InvalidValueException>(() => driver.Move(-1), "negative target rejected");
                    Throws<ASCOM.InvalidValueException>(() => driver.Move(60001), "target above advertised limit rejected");
                    Check(device.Moves == 0, "invalid target never reaches wire");
                    driver.Move(0); driver.Move(60000); Check(device.Position == 60000, "both inclusive position boundaries");
                    Check(driver.IsMoving, "real STATUS moving field");
                    driver.Halt(); Check(!driver.IsMoving, "HALT completion");
                    Check(driver.DeviceState.Count == 3, "single STATUS snapshot projects DeviceState");
                    // Concurrent polling and reads would consume one another's replies without a transaction lock.
                    Exception failure = null;
                    Thread[] threads = new Thread[8];
                    for (int i = 0; i < threads.Length; i++) {
                        int worker = i;
                        threads[i] = new Thread(() => { try {
                            for (int n = 0; n < 40; n++) {
                                if (worker % 2 == 0) { if (driver.Position != 60000) throw new Exception("wrong POS reply"); }
                                else if (driver.IsMoving) throw new Exception("wrong STATUS reply");
                            }
                        } catch (Exception e) { Interlocked.CompareExchange(ref failure, e, null); } });
                        threads[i].Start();
                    }
                    foreach (Thread thread in threads) thread.Join();
                    Check(failure == null, "320 concurrent driver transactions retain response identity");
                    device.StatusReply = "STATUS pos=1 moving=0";
                    Throws<ASCOM.DriverException>(() => { bool x = driver.IsMoving; }, "missing STATUS fields are not defaults");
                    device.StatusReply = "STATUS pos=1 rate=2 volt=3 moving=7";
                    Throws<ASCOM.DriverException>(() => { bool x = driver.IsMoving; }, "invalid moving rejected");
                    device.StatusReply = null;
                    Throws<ASCOM.DriverException>(() => driver.CommandString("POS?\nGOTO 1", false), "command injection rejected");
                    driver.Connected = false; Check(!driver.Connected, "disconnect releases session");
                    Check(device.Closed.WaitOne(2000), "peer observes resource closure");
                    Throws<ASCOM.NotConnectedException>(() => driver.Halt(), "disconnected write");
                }
                using (FakeDevice device = new FakeDevice()) {
                    device.PingReply = "ERR STATE";
                    using (REAFocusDriver driver = Driver(device)) {
                        Throws<ASCOM.DriverException>(() => driver.Connected = true, "failed handshake mapped to ASCOM error");
                        Check(!driver.Connected && device.Closed.WaitOne(2000), "failed local handshake owns and closes socket");
                    }
                }
                using (FakeDevice device = new FakeDevice()) {
                    device.SilentPing = true;
                    using (REAFocusDriver driver = Driver(device)) {
                        Throws<ASCOM.DriverException>(() => driver.Connected = true, "read timeout mapped to ASCOM error");
                        Check(!driver.Connected && device.Closed.WaitOne(4000), "timeout closes socket and leaves disconnected");
                    }
                }
                int unused;
                TcpListener reserve = new TcpListener(IPAddress.Loopback, 0); reserve.Start(); unused = ((IPEndPoint)reserve.LocalEndpoint).Port; reserve.Stop();
                using (REAFocusDriver driver = new REAFocusDriver(new Dictionary<string, string> { { "TcpEndpoint", "127.0.0.1:" + unused } }))
                    Throws<ASCOM.DriverException>(() => driver.Connected = true, "connection refused mapped to ASCOM error");
                using (REAFocusDriver driver = new REAFocusDriver(new Dictionary<string, string>()))
                    Throws<ASCOM.DriverException>(() => driver.Connected = true, "empty configuration does not probe serial hardware");
                using (FakeDevice device = new FakeDevice())
                using (REAFocusDriver driver = Driver(device)) {
                    device.AllowPing = new ManualResetEvent(false);
                    System.Diagnostics.Stopwatch clock = System.Diagnostics.Stopwatch.StartNew();
                    driver.Connect(); clock.Stop();
                    Check(clock.ElapsedMilliseconds < 500 && device.PingSeen.WaitOne(2000) && driver.Connecting,
                          "V4 Connect returns before blocked handshake; Connecting remains responsive");
                    device.AllowPing.Set(); WaitConnection(driver);
                    Check(driver.Connected, "V4 async connect completes");
                    driver.Disconnect(); WaitConnection(driver);
                    Check(!driver.Connected && device.Closed.WaitOne(2000), "V4 async disconnect closes peer");
                }
                using (FakeDevice device = new FakeDevice())
                using (REAFocusDriver driver = Driver(device)) {
                    device.PingReply = "ERR STATE";
                    driver.Connect();
                    Throws<ASCOM.DriverException>(() => WaitConnection(driver), "V4 Connecting reports async handshake error");
                    Check(!driver.Connected && device.Closed.WaitOne(2000), "V4 failed async handshake releases ownership");
                }
                using (FakeDevice device = new FakeDevice()) {
                    REAFocusDriver driver = Driver(device);
                    device.AllowPing = new ManualResetEvent(false);
                    driver.Connect();
                    Check(device.PingSeen.WaitOne(2000), "dispose test reaches in-flight handshake");
                    Thread disposer = new Thread(driver.Dispose); disposer.Start();
                    device.AllowPing.Set();
                    Check(disposer.Join(3000) && !driver.Connected && device.Closed.WaitOne(2000),
                          "Dispose joins in-flight async connection and closes late resource");
                }
                // Direct FP1 callers receive the same transaction guarantees as the driver.
                using (FakeDevice device = new FakeDevice())
                using (Fp1Client client = Fp1Client.OpenTcp("127.0.0.1", device.Port)) {
                    Exception failure = null;
                    Thread a = new Thread(() => { try { for (int n = 0; n < 60; n++) if (client.Position() != 1234) throw new Exception("POS"); } catch (Exception e) { failure = e; } });
                    Thread b = new Thread(() => { try { for (int n = 0; n < 60; n++) if (client.Status().Position != 1234) throw new Exception("STATUS"); } catch (Exception e) { failure = e; } });
                    a.Start(); b.Start(); a.Join(); b.Join(); Check(failure == null, "120 direct-client concurrent transactions");
                    client.Dispose(); Check(!client.IsOpen, "dispose is idempotent and observable");
                    Throws<Fp1Exception>(() => client.Position(), "disposed client cannot write");
                }
                Console.WriteLine(checks + " ASCOM offline checks passed"); return 0;
            }
            catch (Exception e) { Console.Error.WriteLine(e); return 1; }
        }
        private static void WaitConnection(REAFocusDriver driver)
        {
            DateTime deadline = DateTime.UtcNow.AddSeconds(4);
            while (driver.Connecting) {
                if (DateTime.UtcNow > deadline) throw new Exception("connection transition did not finish");
                Thread.Sleep(1);
            }
        }
        private sealed class FakeDevice : IDisposable
        {
            private readonly TcpListener listener;
            private readonly Thread thread;
            private TcpClient active;
            public int Port;
            public volatile int Position = 1234;
            public volatile bool Moving;
            public int Moves;
            public string PingReply = "PONG YacaoFocus 1.001";
            public string StatusReply;
            public bool SilentPing;
            public ManualResetEvent AllowPing;
            public readonly ManualResetEvent PingSeen = new ManualResetEvent(false);
            public readonly ManualResetEvent Closed = new ManualResetEvent(false);
            public FakeDevice() {
                listener = new TcpListener(IPAddress.Loopback, 0); listener.Start();
                Port = ((IPEndPoint)listener.LocalEndpoint).Port;
                thread = new Thread(Listen); thread.IsBackground = true; thread.Start();
            }
            private void Listen() {
                try {
                    using (TcpClient client = listener.AcceptTcpClient()) {
                        active = client;
                        using (StreamReader reader = new StreamReader(client.GetStream(), Encoding.ASCII))
                        using (StreamWriter writer = new StreamWriter(client.GetStream(), Encoding.ASCII) { AutoFlush = true, NewLine = "\n" }) {
                            string command;
                            while ((command = reader.ReadLine()) != null) {
                                if (command == "PING") { PingSeen.Set(); if (AllowPing != null) AllowPing.WaitOne(4000); }
                                if (command == "PING" && SilentPing) continue;
                                string reply;
                                if (command == "PING") reply = PingReply;
                                else if (command == "POS?") reply = "POS " + Position;
                                else if (command == "STATUS?") reply = StatusReply ?? ("STATUS pos=" + Position + " rate=250 volt=12100 moving=" + (Moving ? "1" : "0"));
                                else if (command.StartsWith("GOTO ", StringComparison.Ordinal)) { Position = int.Parse(command.Substring(5), CultureInfo.InvariantCulture); Moving = true; Moves++; reply = "OK"; }
                                else if (command == "HALT") { Moving = false; reply = "OK"; }
                                else reply = "ERR UNKNOWN";
                                writer.WriteLine(reply);
                            }
                        }
                    }
                } catch (IOException) { } catch (SocketException) { } catch (ObjectDisposedException) { }
                finally { Closed.Set(); }
            }
            public void Dispose() { listener.Stop(); if (active != null) active.Close(); thread.Join(4000); Closed.Dispose(); PingSeen.Dispose(); if (AllowPing != null) AllowPing.Dispose(); }
        }
    }
}
