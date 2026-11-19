# REA focuser ASCOM driver

Self-contained FP1 driver for `IFocuserV2/V3/V4` (net472), advertising
`InterfaceVersion=4`. Does not depend on the Python bridge.

**Verified:** managed interface bindings and fake loopback TCP device only.
**Not verified:** COM activation/registration, ASCOM conformance tools, physical
serial/USB devices or any particular imaging client. No compatibility certification.

## Coordinates and capabilities

- `Move(position)` sends `GOTO`, rejecting values outside `0..MaxStep` before I/O.
- `Absolute=true` means step-coordinate addressing in the current power-on epoch.
  No homing, limits or persistent position are implemented. Establish a safe origin
  separately; the configured `MaxStep` is not a physical safety limit.
- `StepSize` and `Temperature` throw `PropertyNotImplementedException`: physical
  micrometres-per-step and temperature are unknown. `TempCompAvailable=false`.
- `IsMoving` consumes the actual FP1 motor state, not the configured rate.
- One device session owns all transactions and close operations. Each FP1 command
  locks the whole write/read transaction; timeout closes the stream so a late reply
  cannot be consumed by the next request. Reconnect explicitly after an I/O failure.
- Failed handshakes dispose locally owned resources and report ASCOM errors.
- V4 `Connect/Disconnect` are nonblocking; `Connecting` reports progress and throws
  an ASCOM error on async failure. Legacy `Connected` writes remain synchronous.
  `Dispose` waits for an in-flight transition and closes late-arriving resources.

## Build and offline tests

Provide these already-installed ASCOM assemblies via `AscomDir`:
`ASCOM.DeviceInterfaces.dll`, `ASCOM.Exceptions.dll`, `ASCOM.Utilities.dll`,
`ASCOM.SettingsProvider.dll`, `ASCOM.Internal.Extensions.dll`, `ASCOM.Attributes.dll`.
Also requires the .NET Framework 4.7.2 targeting pack and .NET SDK. No new packages
are installed by the offline runner.

From the project root:

```powershell
dotnet build host/AscomFocuser/REAFocusDriverTest.csproj -c Release -p:AscomDir="C:\path\to\ascom\assemblies"
.\host\AscomFocuser\bin\Release\REAFocusDriverTest.exe
# All firmware/bridge/driver suites:
.\test\run_offline.ps1 -AscomDir "C:\path\to\ascom\assemblies"
```

The 37-check executable uses a directly constructed **managed** driver and an
in-memory configuration dictionary; no registry reads/writes, COM registration,
serial enumeration or real hardware access. It checks 440 concurrent driver/client
transactions, position bounds, unknown capabilities, error mapping, handshake
failure, timeouts, peer-observed socket cleanup and V4 async progress/failure/disposal. This replaces the original
registered-COM test, which mutated the user's registry and tested incorrect version
and step-size expectations.

## Explicit configuration

Normal COM activation reads (never creates) `HKCU\Software\REA\Focuser`:

| Value | Meaning | Default |
|---|---|---|
| `Port` | Explicit serial device | none; connect fails rather than scanning unrelated devices |
| `Baud` | Baud rate for that serial device | 115200 |
| `TcpEndpoint` | `host:port`, overrides `Port` | none |
| `MaxStep` | Host target range / maximum increment | 60000 |

The managed `REAFocusDriver(IDictionary<string,string>)` overload accepts the same
settings without registry access, for offline testing or explicit managed use.

## Optional registration (not performed by tests)

`ProgID=REAFocus.Focuser`, CLSID `{7B2A5A64-0E0F-4C6E-9F2C-2E8B4A1D6C31}`.
For a separately authorized deployment, `register.cmd` invokes regasm and the ASCOM
Profile API; `unregister.cmd` reverses it. The scripts currently target 64-bit
Framework registration. Verify client/platform architecture before deployment.
Registration errors now propagate rather than logging and reporting success.
Do not run either script as part of offline verification.
