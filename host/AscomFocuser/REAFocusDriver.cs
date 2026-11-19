using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Ports;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;
using ASCOM.DeviceInterface;
using Microsoft.Win32;

namespace REA.Focus
{
    /// <summary>
    /// ASCOM driver for the REA focus controller (the restored "ye cao" focuser board).
    /// </summary>
    /// <remarks>
    /// <para>
    /// The driver speaks FP1, the line protocol implemented by the reconstructed firmware,
    /// over a serial port (USB CDC virtual COM port or the USART3 header) or over TCP.
    /// Implements IFocuserV2/V3/V4. Offline tests exercise managed bindings,
    /// not registration or compatibility with particular imaging clients.
    /// </para>
    /// <para>
    /// Settings are kept under HKEY_CURRENT_USER\Software\REA\Focuser:
    /// Port (explicit COM port name; no automatic probing),
    /// Baud (default 115200), TcpEndpoint ("host:port", optional - takes precedence over
    /// the serial port) and MaxStep (default 60000).
    /// </para>
    /// </remarks>
    [ComVisible(true)]
    [Guid(REAFocusDriver.Clsid)]
    [ProgId(REAFocusDriver.ProgIdValue)]
    [ClassInterface(ClassInterfaceType.None)]
    public class REAFocusDriver : IFocuserV2, IFocuserV3, IFocuserV4, IDisposable
    {
        public const string Clsid = "7B2A5A64-0E0F-4C6E-9F2C-2E8B4A1D6C31";
        public const string ProgIdValue = "REAFocus.Focuser";
        public const string DriverDescription = "REA focus controller for the restored ye cao focuser board.";

        private const string SettingsKey = @"Software\REA\Focuser";
        private const int DefaultBaud = 115200;
        private const int DefaultMaxStep = 60000;

        private readonly object _gate = new object();
        private readonly object _connectionGate = new object();
        private System.Threading.Tasks.Task _connectionWork;
        private Exception _connectionError;
        private bool _connecting;
        private volatile bool _disposed;
        private readonly IDictionary<string, string> _settings;
        private Fp1Client _client;
        private bool _connected;
        private string _activeEndpoint = string.Empty;

        public REAFocusDriver() { }

        // Explicit configuration also enables offline tests without registry writes.
        public REAFocusDriver(IDictionary<string, string> settings)
        {
            _settings = new Dictionary<string, string>(settings);
        }

        #region settings

        private static RegistryKey Settings
        {
            get { return Registry.CurrentUser.OpenSubKey(SettingsKey, false); }
        }

        private string ReadSetting(string name, string fallback)
        {
            if (_settings != null) { string value; return _settings.TryGetValue(name, out value) ? value : fallback; }
            using (RegistryKey key = Settings)
            {
                if (key == null)
                {
                    return fallback;
                }

                object value = key.GetValue(name);
                if (value == null)
                {
                    return fallback;
                }

                string text = value.ToString();
                return string.IsNullOrEmpty(text) ? fallback : text;
            }
        }

        private int ReadSetting(string name, int fallback)
        {
            if (_settings != null) { int number; return int.TryParse(ReadSetting(name, ""), out number) ? number : fallback; }
            using (RegistryKey key = Settings)
            {
                if (key == null)
                {
                    return fallback;
                }

                object value = key.GetValue(name);
                if (value == null)
                {
                    return fallback;
                }

                int number;
                if (int.TryParse(value.ToString(), NumberStyles.Integer, CultureInfo.InvariantCulture, out number))
                {
                    return number;
                }
            }

            return fallback;
        }

        #endregion

        #region connection

        private void OpenDevice()
        {
            lock (_gate)
            {
                if (_disposed) throw new ASCOM.DriverException("Driver has been disposed.");
                OpenDeviceLocked();
            }
        }

        private void OpenDeviceLocked()
        {
            if (_connected && _client != null && _client.IsOpen) return;
            CloseDevice();

            string endpoint = ReadSetting("TcpEndpoint", string.Empty);
            Fp1Client client = null;
            try
            {
                if (!string.IsNullOrEmpty(endpoint))
                {
                    string[] parts = endpoint.Split(':');
                    if (parts.Length != 2)
                    {
                        throw new Fp1Exception("TcpEndpoint must look like host:port");
                    }

                    int port;
                    if (string.IsNullOrWhiteSpace(parts[0]) || !int.TryParse(parts[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out port) || port < 1 || port > 65535)
                    {
                        throw new Fp1Exception("TcpEndpoint must look like host:port");
                    }

                    client = Fp1Client.OpenTcp(parts[0], port);
                }
                else
                {
                    string portName = ReadSetting("Port", string.Empty);
                    if (string.IsNullOrEmpty(portName))
                    {
                        throw new Fp1Exception("Configure Port or TcpEndpoint explicitly; automatic probing is disabled to protect unrelated hardware.");
                    }

                    client = Fp1Client.OpenSerial(portName, ReadSetting("Baud", DefaultBaud));
                }

                client.Ping();
            }
            catch (Exception error)
            {
                try { if (client != null) client.Dispose(); }
                finally { _client = null; _connected = false; _activeEndpoint = string.Empty; }
                if (!(error is Fp1Exception || error is IOException || error is UnauthorizedAccessException ||
                      error is TimeoutException || error is System.Net.Sockets.SocketException || error is ArgumentException)) throw;
                throw new ASCOM.DriverException("Cannot reach the REA focuser: " + error.Message, error);
            }

            _client = client;
            _connected = true;
            _activeEndpoint = client.Endpoint;
        }

        private void CloseDevice()
        {
            lock (_gate)
            {
                Fp1Client client = _client;
                _client = null;
                _connected = false;
                _activeEndpoint = string.Empty;
                if (client != null) client.Dispose();
            }
        }

        private T DeviceCall<T>(string operation, Func<Fp1Client, T> call)
        {
            lock (_gate)
            {
                CheckConnected(operation);
                try { return call(_client); }
                catch (Exception error)
                {
                    if (!(error is Fp1Exception || error is IOException || error is TimeoutException || error is System.Net.Sockets.SocketException)) throw;
                    if (_client != null && !_client.IsOpen) CloseDevice();
                    throw new ASCOM.DriverException(operation + " failed: " + error.Message, error);
                }
            }
        }

        private void CheckConnected(string operation)
        {
            if (!_connected || _client == null || !_client.IsOpen)
            {
                throw new ASCOM.NotConnectedException("The REA focuser is not connected; " + operation + " needs a connected device.");
            }
        }

        #endregion

        #region common properties

        public bool Connected
        {
            get { lock (_gate) { return _connected && _client != null && _client.IsOpen; } }
            set
            {
                lock (_connectionGate)
                {
                    if (_connecting) throw new ASCOM.DriverException("A connection transition is already in progress.");
                    _connectionError = null;
                }
                if (value)
                {
                    OpenDevice();
                }
                else
                {
                    CloseDevice();
                }
            }
        }

        public string Description
        {
            get { return DriverDescription; }
        }

        public string DriverInfo
        {
            get
            {
                Version version = typeof(REAFocusDriver).Assembly.GetName().Version;
                return "REA focus controller ASCOM driver. Version: "
                    + version.Major.ToString(CultureInfo.InvariantCulture)
                    + "."
                    + version.Minor.ToString(CultureInfo.InvariantCulture)
                    + ". Protocol: FP1 over "
                    + (_connected ? _activeEndpoint : "the configured port")
                    + ".";
            }
        }

        public string DriverVersion
        {
            get
            {
                Version version = typeof(REAFocusDriver).Assembly.GetName().Version;
                return version.Major.ToString(CultureInfo.InvariantCulture)
                    + "."
                    + version.Minor.ToString(CultureInfo.InvariantCulture);
            }
        }

        public short InterfaceVersion
        {
            get { return 4; }
        }

        public string Name
        {
            get { return "REA Focuser"; }
        }

        public ArrayList SupportedActions
        {
            get { return new ArrayList(); }
        }

        public void SetupDialog()
        {
            string message = "REA focus controller" + Environment.NewLine + Environment.NewLine
                + "Serial port: " + ReadSetting("Port", "(not configured)") + Environment.NewLine
                + "TCP endpoint: " + ReadSetting("TcpEndpoint", "(none)") + Environment.NewLine
                + "Baud rate: " + ReadSetting("Baud", DefaultBaud).ToString(CultureInfo.InvariantCulture) + Environment.NewLine + Environment.NewLine
                + "Settings are stored under HKEY_CURRENT_USER\\" + SettingsKey + ".";
            MessageBox.Show(message, "REA Focuser", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }

        public string Action(string actionName, string actionParameters)
        {
            throw new ASCOM.ActionNotImplementedException("Action " + actionName + " is not implemented by this driver.");
        }

        public void CommandBlind(string command, bool raw)
        {
            DeviceCall("CommandBlind", c => c.Command(command));
        }

        public bool CommandBool(string command, bool raw)
        {
            return DeviceCall("CommandBool", c => c.Command(command)) == "OK";
        }

        public string CommandString(string command, bool raw)
        {
            return DeviceCall("CommandString", c => c.Command(command));
        }

        public void Dispose()
        {
            System.Threading.Tasks.Task work;
            lock (_connectionGate) { _disposed = true; work = _connectionWork; }
            if (work != null) work.GetAwaiter().GetResult();
            CloseDevice();
        }

        #endregion

        #region focuser properties

        public bool Absolute
        {
            get { return true; }
        }

        public bool IsMoving
        {
            get
            {
                return DeviceCall("IsMoving", c => c.Status().Moving);
            }
        }

        public bool Link
        {
            get { lock (_gate) { return _connected && _client != null && _client.IsOpen; } }
            set { Connected = value; }
        }

        public int MaxIncrement
        {
            get { return Math.Max(1, ReadSetting("MaxStep", DefaultMaxStep)); }
        }

        public int MaxStep
        {
            get { return Math.Max(1, ReadSetting("MaxStep", DefaultMaxStep)); }
        }

        public int Position
        {
            get
            {
                return DeviceCall("Position", c => c.Position());
            }

            set
            {
                Move(value);
            }
        }

        public double StepSize
        {
            get { throw new ASCOM.PropertyNotImplementedException("StepSize"); }
        }

        public bool TempComp
        {
            get { return false; }
            set { throw new ASCOM.PropertyNotImplementedException("TempComp"); }
        }

        public bool TempCompAvailable
        {
            get { return false; }
        }

        public double Temperature
        {
            get { throw new ASCOM.PropertyNotImplementedException("Temperature"); }
        }

        public void Halt()
        {
            DeviceCall("Halt", c => { c.Halt(); return true; });
        }

        public void Move(int position)
        {
            DeviceCall("Move", c => {
                if (position < 0 || position > MaxStep)
                    throw new ASCOM.InvalidValueException("Position", position.ToString(CultureInfo.InvariantCulture), "0.." + MaxStep);
                c.MoveTo(position); return true;
            });
        }

        #endregion

        #region ASCOM registration

        /// <summary>
        /// Registers the driver with the ASCOM chooser. This runs automatically when
        /// the assembly is registered for COM interop, so regasm is enough.
        /// </summary>
        [ComRegisterFunction]
        public static void RegisterAscom(Type type)
        {
            try
            {
                using (ASCOM.Utilities.Profile profile = new ASCOM.Utilities.Profile())
                {
                    profile.DeviceType = "Focuser";
                    profile.Register(REAFocusDriver.ProgIdValue, REAFocusDriver.DriverDescription);
                }
            }
            catch (Exception error)
            {
                throw new InvalidOperationException("ASCOM chooser registration failed", error);
            }
        }

        /// <summary>
        /// Removes the driver from the ASCOM chooser.
        /// </summary>
        [ComUnregisterFunction]
        public static void UnregisterAscom(Type type)
        {
            try
            {
                using (ASCOM.Utilities.Profile profile = new ASCOM.Utilities.Profile())
                {
                    profile.DeviceType = "Focuser";
                    profile.Unregister(REAFocusDriver.ProgIdValue);
                }
            }
            catch (Exception error)
            {
                throw new InvalidOperationException("ASCOM chooser unregistration failed", error);
            }
        }

        #endregion

        #region IFocuserV4 extras

        public bool Connecting
        {
            get
            {
                lock (_connectionGate)
                {
                    if (_connectionError != null)
                        throw new ASCOM.DriverException("Asynchronous connection transition failed: " + _connectionError.Message, _connectionError);
                    return _connecting;
                }
            }
        }

        private void StartConnection(bool connect)
        {
            lock (_connectionGate)
            {
                if (_disposed) throw new ASCOM.DriverException("Driver has been disposed.");
                if (_connecting) throw new ASCOM.DriverException("A connection transition is already in progress.");
                _connectionError = null;
                _connecting = true;
                _connectionWork = System.Threading.Tasks.Task.Run(() => {
                    try { if (connect) OpenDevice(); else CloseDevice(); }
                    catch (Exception error) { lock (_connectionGate) { _connectionError = error; } }
                    finally { lock (_connectionGate) { _connecting = false; } }
                });
            }
        }

        public void Connect() { StartConnection(true); }
        public void Disconnect() { StartConnection(false); }

        public IStateValueCollection DeviceState
        {
            get
            {
                return DeviceCall("DeviceState", c => {
                    Fp1Status status = c.Status();
                    StateValueCollection states = new StateValueCollection();
                    states.Add(new StateValue("IsMoving", status.Moving));
                    states.Add(new StateValue("Position", status.Position));
                    states.Add(new StateValue("TimeStamp", DateTime.UtcNow));
                    return (IStateValueCollection)states;
                });
            }
        }

        #endregion
    }
}
