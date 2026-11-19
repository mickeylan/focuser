using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Ports;
using System.Net.Sockets;
using System.Text;

namespace REA.Focus
{
    /// <summary>
    /// Raised when the focuser answers with an error or with something unexpected.
    /// </summary>
    public sealed class Fp1Exception : Exception
    {
        public Fp1Exception(string message) : base(message)
        {
        }
    }

    /// <summary>
    /// Status values reported by the focuser.
    /// </summary>
    public struct Fp1Status
    {
        public int Position;
        public int Rate;
        public int Millivolts;
        public bool Moving;
    }

    /// <summary>
    /// Client for FP1, the line protocol spoken by the REA focus controller.
    /// </summary>
    /// <remarks>
    /// The transport is either a serial port (USB CDC virtual COM port or the USART3
    /// header) or a TCP socket. The TCP option keeps the ASCOM driver testable
    /// without the hardware and allows the controller to sit behind a serial-to-network
    /// bridge. Commands are ASCII lines terminated by a single line feed; the focuser
    /// answers with one line per command.
    /// </remarks>
    public sealed class Fp1Client : IDisposable
    {
        private const int DefaultTimeoutMs = 2000;

        private readonly object _gate = new object();
        private bool _disposed;
        private readonly TextReader _reader;
        private readonly Stream _stream;
        private readonly SerialPort _serial;
        private readonly TcpClient _tcp;
        private readonly string _endpoint;

        private Fp1Client(TextReader reader, Stream stream, SerialPort serial, TcpClient tcp, string endpoint)
        {
            _reader = reader;
            _stream = stream;
            _serial = serial;
            _tcp = tcp;
            _endpoint = endpoint;
        }

        public static Fp1Client OpenSerial(string portName, int baud)
        {
            if (string.IsNullOrEmpty(portName))
            {
                throw new Fp1Exception("no serial port given");
            }

            SerialPort port = new SerialPort(portName, baud, Parity.None, 8, StopBits.One);
            port.NewLine = "\n";
            port.ReadTimeout = DefaultTimeoutMs;
            port.WriteTimeout = DefaultTimeoutMs;
            try
            {
                port.Open();
                return new Fp1Client(new StreamReader(port.BaseStream, Encoding.ASCII), port.BaseStream, port, null, portName);
            }
            catch { port.Dispose(); throw; }
        }

        public static Fp1Client OpenTcp(string host, int port)
        {
            TcpClient client = new TcpClient();
            try
            {
                IAsyncResult pending = client.BeginConnect(host, port, null, null);
                using (System.Threading.WaitHandle wait = pending.AsyncWaitHandle)
                {
                    if (!wait.WaitOne(DefaultTimeoutMs)) throw new TimeoutException("TCP connect timed out");
                    client.EndConnect(pending);
                }
                NetworkStream stream = client.GetStream();
                stream.ReadTimeout = DefaultTimeoutMs;
                stream.WriteTimeout = DefaultTimeoutMs;
                return new Fp1Client(new StreamReader(stream, Encoding.ASCII), stream, null, client, host + ":" + port.ToString(CultureInfo.InvariantCulture));
            }
            catch { client.Close(); throw; }
        }

        public bool IsOpen
        {
            get
            {
                lock (_gate)
                {
                    if (_disposed) return false;
                    if (_serial != null) return _serial.IsOpen;
                    return _tcp != null && _tcp.Connected;
                }
            }
        }

        public string Endpoint
        {
            get { return _endpoint; }
        }

        /// <summary>
        /// Sends one command and returns the answer line. Error answers raise
        /// <see cref="Fp1Exception"/>.
        /// </summary>
        public string Command(string command)
        {
            lock (_gate)
            {
                if (string.IsNullOrWhiteSpace(command) || command.IndexOfAny(new char[] { '\r', '\n' }) >= 0)
                    throw new Fp1Exception("one command line required");
                try { return CommandLocked(command); }
                catch (IOException) { Dispose(); throw; }
                catch (TimeoutException) { Dispose(); throw; }
                catch (SocketException) { Dispose(); throw; }
            }
        }

        private string CommandLocked(string command)
        {
            if (!IsOpen)
            {
                throw new Fp1Exception("not connected to the focuser");
            }

            byte[] payload = Encoding.ASCII.GetBytes(command + "\n");
            _stream.Write(payload, 0, payload.Length);
            _stream.Flush();

            string line = _reader.ReadLine();
            if (line == null)
            {
                throw new Fp1Exception("no answer to " + command);
            }

            line = line.Trim();
            if (line.StartsWith("ERR", StringComparison.Ordinal))
            {
                throw new Fp1Exception(line);
            }

            return line;
        }

        public string Ping()
        {
            string reply = Command("PING");
            if (!reply.StartsWith("PONG ", StringComparison.Ordinal))
            {
                throw new Fp1Exception("unexpected answer to PING: " + reply);
            }

            return reply;
        }

        public int Position()
        {
            string reply = Command("POS?");
            if (!reply.StartsWith("POS ", StringComparison.Ordinal))
            {
                throw new Fp1Exception("unexpected answer to POS?: " + reply);
            }

            int value;
            if (!int.TryParse(reply.Substring(4).Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out value))
            {
                throw new Fp1Exception("unexpected answer to POS?: " + reply);
            }

            return value;
        }

        public Fp1Status Status()
        {
            string reply = Command("STATUS?");
            if (!reply.StartsWith("STATUS ", StringComparison.Ordinal))
            {
                throw new Fp1Exception("unexpected answer to STATUS?: " + reply);
            }

            Fp1Status status = new Fp1Status();
            status.Position = 0;
            status.Rate = 0;
            status.Millivolts = 0;
            status.Moving = false;

            HashSet<string> seen = new HashSet<string>();
            string[] parts = reply.Split(new char[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
            for (int i = 1; i < parts.Length; i++)
            {
                string[] pair = parts[i].Split(new char[] { '=' }, 2);
                if (pair.Length != 2 || !seen.Add(pair[0]))
                    throw new Fp1Exception("malformed STATUS: " + reply);

                int number;
                if (pair[0] == "pos" && int.TryParse(pair[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out number))
                {
                    status.Position = number;
                }
                else if (pair[0] == "rate" && int.TryParse(pair[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out number))
                {
                    status.Rate = number;
                }
                else if (pair[0] == "volt" && int.TryParse(pair[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out number))
                {
                    status.Millivolts = number;
                }
                else if (pair[0] == "moving")
                {
                    if (pair[1] != "0" && pair[1] != "1") throw new Fp1Exception("invalid moving: " + reply);
                    status.Moving = pair[1] == "1";
                }
                else throw new Fp1Exception("invalid STATUS field: " + reply);
            }
            if (!seen.SetEquals(new string[] { "pos", "rate", "volt", "moving" }))
                throw new Fp1Exception("incomplete STATUS: " + reply);
            return status;
        }

        public void MoveTo(int position)
        {
            string reply = Command("GOTO " + position.ToString(CultureInfo.InvariantCulture));
            if (reply != "OK")
            {
                throw new Fp1Exception("unexpected answer to GOTO: " + reply);
            }
        }

        public void Halt()
        {
            string reply = Command("HALT");
            if (reply != "OK")
            {
                throw new Fp1Exception("unexpected answer to HALT: " + reply);
            }
        }

        public void Dispose()
        {
            lock (_gate)
            {
                if (_disposed) return;
                _disposed = true;
                try { _reader.Dispose(); }
                finally
                {
                    try { if (_serial != null) _serial.Dispose(); }
                    finally { if (_tcp != null) _tcp.Close(); }
                }
            }
        }
    }
}
