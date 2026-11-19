@echo off
rem Registers the REA focuser ASCOM driver: COM class + ASCOM chooser entry.
rem Run this from an elevated command prompt.
setlocal
set DRIVER=%~dp0bin\Release\REAFocus.dll
if not exist "%DRIVER%" set DRIVER=%~dp0REAFocus.dll
if not exist "%DRIVER%" (
  echo REAFocus.dll not found. Build the driver first, see README.md.
  exit /b 1
)
"%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\regasm.exe" "%DRIVER%" /codebase
if errorlevel 1 exit /b 1
echo.
echo The REA focuser is now registered. It shows up in the ASCOM chooser as
echo "REA Focuser" under the Focuser tab.
endlocal