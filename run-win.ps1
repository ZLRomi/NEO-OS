# NEO-OS-3 Windows test runner (QEMU + OVMF, native SDL window).
# Usage: powershell -ExecutionPolicy Bypass -File .\run-win.ps1 [sdl|gtk] [tcg] [log]
#
# Root cause of the "keyboard works, then stops" saga (fixed in kernel):
#   interrupts::pic_init wrote master IMR 0xEC, which keeps IRQ2 (cascade)
#   masked. IRQ12 (mouse) therefore never reached the CPU and stale PS/2 aux
#   bytes clogged the shared 8042 queue, taking keyboard input down with it.
#   Correct mask is 0xE8. Not focus, not IME, not SDL/VNC.
#
# Run in a NORMAL, VISIBLE PowerShell window: launching QEMU with a hidden
# window leaves MainWindowHandle=0 and no top-level window at all.
#
# The guest drives a real i8042 PS/2 keyboard AND mouse. QEMU's HMP monitor
# is exposed on 127.0.0.1:4555 so `sendkey`, `mouse_move` and `mouse_button`
# can inject the same events for GUI/mouse development and automation.
# No VNC, no serial-terminal workaround: both would cut off mouse input.
$ErrorActionPreference = "Stop"

$Qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
if (-not (Test-Path -LiteralPath $Qemu)) {
    throw "QEMU not found at $Qemu (install qemu-w64-setup.exe first)"
}

# Single instance: two VMs fight over esp.img, serial.log and the monitor port.
$Existing = Get-Process -Name "qemu-system-x86_64" -ErrorAction SilentlyContinue
if ($Existing) {
    throw "Another QEMU is already running (PID $($Existing.Id)). Close all QEMU windows first, then rerun."
}

# Firmware: the edk2-ovmf 4m set proven with this image. Verified layout:
#   $env:TEMP\opencode\OVMF_CODE.4m.fd  (readonly pflash)
#   $env:TEMP\opencode\OVMF_VARS.4m.fd  (writable pflash)
# Get them from WSL: cp /usr/share/edk2-ovmf/x64/OVMF_*.4m.fd <temp>/opencode/
# NOTE: qemu-w64's own edk2-x86_64-code.fd does NOT boot via -bios here,
# so don't fall back to it.
$FwDir = Join-Path $env:TEMP "opencode"
$CodeFd = Join-Path $FwDir "OVMF_CODE.4m.fd"
$VarsFd = Join-Path $FwDir "OVMF_VARS.4m.fd"
if (-not (Test-Path -LiteralPath $CodeFd)) { throw "missing $CodeFd (see note above)" }
if (-not (Test-Path -LiteralPath $VarsFd)) { throw "missing $VarsFd (see note above)" }

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Esp = Join-Path $Root "esp.img"
if (-not (Test-Path -LiteralPath $Esp)) { throw "esp.img missing, build it in WSL first: ./tools/mkesp.sh" }

$SerialLog = Join-Path $Root "serial.log"
Remove-Item -LiteralPath $SerialLog -ErrorAction SilentlyContinue

# Accelerator: WHPX (Hyper-V platform) boots in ~1s; fall back to TCG if the
# feature isn't enabled. TCG makes everything sluggish (the M5 "lag" report).
# Display: sdl (default) or gtk. Both are native QEMU windows, not VNC.
$Accel = "whpx"
$Display = "sdl"
$UseLog = $false
foreach ($a in $args) {
    if ($a -eq "tcg") { $Accel = "tcg" }
    if ($a -eq "sdl") { $Display = "sdl" }
    if ($a -eq "gtk") { $Display = "gtk" }
    if ($a -eq "log") { $UseLog = $true }
}

$Serial = "file:$SerialLog"
if ($UseLog) {
    Write-Host "COM : file $SerialLog (no interactive input in this mode)"
}

$Args = @("-machine", "q35", "-cpu", "qemu64", "-m", "512",
          "-accel", $Accel,
          "-drive", "if=pflash,format=raw,readonly=on,file=$CodeFd",
          "-drive", "if=pflash,format=raw,file=$VarsFd",
          "-drive", "file=$Esp,format=raw",
          "-serial", $Serial,
          "-monitor", "tcp:127.0.0.1:4555,server=on,wait=off",
          "-display", $Display)

Write-Host "FW : $CodeFd"
Write-Host "ESP: $Esp"
Write-Host "ACC: $Accel"
Write-Host "DSP: $Display (native window; click it, IME -> English)"
Write-Host "MON: tcp 127.0.0.1:4555 (sendkey / mouse_move / mouse_button)"
& $Qemu @Args
if ($LASTEXITCODE -ne 0 -and $Accel -eq "whpx") {
    Write-Host "WHPX failed (is HypervisorPlatform enabled?); retrying with TCG..."
    $idx = [Array]::IndexOf($Args, "whpx")
    $Args[$idx] = "tcg"
    & $Qemu @Args
}
