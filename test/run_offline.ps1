param([string]$AscomDir = "$PSScriptRoot/../host/AscomFocuser/bin/Release")
# Run from repository root. No COM registration, registry mutation or real serial I/O.
$ErrorActionPreference = 'Stop'
function Run([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
$flags = @('-std=c99', '-D_CRT_SECURE_NO_WARNINGS', '-Wall', '-Wextra', '-Wpedantic', '-Werror')
Run 'clang' ($flags + @('-o', 'test/motor_test.exe', 'test/test_focus_motor.c', 'src/recovered/focus_motor.c'))
Run './test/motor_test.exe' @()
Run 'clang' ($flags + @('-o', 'test/protocol_test.exe', 'test/test_focus_protocol.c', 'src/protocol/focus_protocol.c'))
Run './test/protocol_test.exe' @()
Run 'clang' ($flags + @('-Itest/hal_fake', '-o', 'test/board_test.exe', 'test/test_focus_board.c', 'src/recovered/board_stm32f1.c', 'src/recovered/focus_motor.c', 'src/protocol/focus_protocol.c'))
Run './test/board_test.exe' @()
Run 'clang' ($flags + @('-o', 'test/focus_simulator.exe', 'test/focus_simulator.c', 'src/recovered/focus_motor.c', 'src/protocol/focus_protocol.c'))
Run 'python' @('-m', 'py_compile', 'host/focus_calibrate.py')
Run 'python' @('host/focus_calibrate.py', '--help')
Run 'python' @('host/test_focus_bridge.py')
Run 'python' @('host/test_firmware_bridge.py')
Run 'dotnet' @('build', 'host/AscomFocuser/REAFocusDriverTest.csproj', '-c', 'Release', "-p:AscomDir=$AscomDir", '--no-restore')
Run './host/AscomFocuser/bin/Release/REAFocusDriverTest.exe' @()
Write-Output 'ALL OFFLINE SUITES PASSED (no real hardware / COM registration)'
