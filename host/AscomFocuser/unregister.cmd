@echo off
rem Removes the REA focuser ASCOM driver from COM and from the ASCOM chooser.
rem Run this from an elevated command prompt.
setlocal
set DRIVER=%~dp0bin\Release\REAFocus.dll
if not exist "%DRIVER%" set DRIVER=%~dp0REAFocus.dll
if not exist "%DRIVER%" (
  echo REAFocus.dll not found.
  exit /b 1
)
"%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\regasm.exe" "%DRIVER%" /unregister /codebase
endlocal