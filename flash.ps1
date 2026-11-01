# Jednoklikové nahrání firmwaru Panelu světel do Waveshare ESP32-S3-Touch-AMOLED-2.16
param(
    [string]$Port = "",
    [switch]$Monitor,
    [switch]$EraseFirst,
    [switch]$BuildOnly
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

if (-not (Test-Path "include\secrets.h")) {
    throw "Chybí include\secrets.h - zkopíruj include\secrets.example.h a vyplň Wi-Fi, účet Tapo a adresy světel."
}

# PlatformIO: proměnná PANEL_PIO, příkaz pio v PATH, instalace v ~/.platformio, nebo python -m platformio
function Get-PioCommand {
    if ($env:PANEL_PIO -and (Test-Path $env:PANEL_PIO)) { return @($env:PANEL_PIO) }
    $cmd = Get-Command pio -ErrorAction SilentlyContinue
    if ($cmd) { return @($cmd.Source) }
    $penv = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
    if (Test-Path $penv) { return @($penv) }
    foreach ($py in @((Join-Path $env:LOCALAPPDATA "Programs\Python\Python312\python.exe"), "python")) {
        try {
            & $py -c "import platformio" 2>$null
            if ($LASTEXITCODE -eq 0) { return @($py, "-m", "platformio") }
        } catch { }
    }
    throw "PlatformIO nenalezeno. Nainstaluj: python -m pip install platformio (nebo nastav PANEL_PIO na cestu k pio.exe)."
}

$pio = @(Get-PioCommand)
$pioExe = $pio[0]
$pioPre = @()
if ($pio.Count -gt 1) { $pioPre = $pio[1..($pio.Count - 1)] }

$portArgs = @()
$monitorArgs = @()
if ($Port -ne "") {
    $portArgs = @("--upload-port", $Port)
    $monitorArgs = @("--port", $Port)
}

# Nativní příkazy nevyhazují výjimku, chybový kód je nutné hlídat ručně
function Assert-ExitCode([string]$Step) {
    if ($LASTEXITCODE -ne 0) { throw "$Step selhalo (exit code $LASTEXITCODE)." }
}

Write-Host "=== 1/3 Kompiluji firmware Panel světel ===" -ForegroundColor Cyan
& $pioExe @pioPre run
Assert-ExitCode "Kompilace"

if ($BuildOnly) {
    Write-Host "Hotovo, jen kompilace (-BuildOnly)." -ForegroundColor Green
    exit 0
}

if ($EraseFirst) {
    Write-Host "=== Mažu flash paměť ESP32-S3 (včetně nastavení) ===" -ForegroundColor Yellow
    & $pioExe @pioPre run -t erase @portArgs
    Assert-ExitCode "Mazání flash"
}

$rozhodovacBridge = Join-Path $PSScriptRoot "..\rozhodovac\bridge"
$flashingLock = Join-Path $rozhodovacBridge ".flashing"
$rozhodovacCtl = Join-Path $rozhodovacBridge "rozhodovac_ctl.py"
$daemonWasStopped = $false

try {
    if (Test-Path $rozhodovacCtl) {
        New-Item -ItemType File -Path $flashingLock -Force | Out-Null
        & python $rozhodovacCtl stop 2>$null
        $daemonWasStopped = $true
    }

    Write-Host "=== 2/3 Nahrávám firmware do krabičky ===" -ForegroundColor Cyan
    & $pioExe @pioPre run -t upload @portArgs
    Assert-ExitCode "Nahrání firmwaru"
} finally {
    if (Test-Path $flashingLock) {
        Remove-Item -Path $flashingLock -Force -ErrorAction SilentlyContinue
    }
    if ($daemonWasStopped) {
        & python $rozhodovacCtl start 2>$null
    }
}

if ($Monitor) {
    Write-Host "=== 3/3 Sériový monitor (ukončení: Ctrl+C) ===" -ForegroundColor Green
    & $pioExe @pioPre device monitor @monitorArgs
} else {
    Write-Host "=== 3/3 Hotovo. Výpisy: .\flash.ps1 -Monitor ===" -ForegroundColor Green
}
