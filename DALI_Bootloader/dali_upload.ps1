# DALI Bootloader Upload Script
# Sends firmware binary to CH32V003 via DALI bus using the Pico master.
#
# Usage: .\dali_upload.ps1 [-BinPath <path>] [-Addr <0-63|broadcast>] [-SkipEntry]
#
# Prerequisites:
#   - Pico DALI master on COM9
#   - CH32V003 running DALI firmware (for cmd 131 entry) or already in bootloader mode (-SkipEntry)

param(
    [string]$BinPath = "..\Firmware\.pio\build\genericCH32V003F4P6\firmware.bin",
    [string]$Addr = "broadcast",     # Short address (0-63) or "broadcast"
    [string]$MasterPort = "COM9",
    [string]$SlavePort = "COM11",
    [switch]$SkipEntry               # Skip cmd 131 if already in bootloader
)

$ErrorActionPreference = "Stop"

# Resolve firmware binary path
$BinPath = Resolve-Path $BinPath -ErrorAction Stop
$binData = [System.IO.File]::ReadAllBytes($BinPath)
Write-Host "Firmware: $BinPath ($($binData.Length) bytes, $([math]::Ceiling($binData.Length / 64)) pages)" -ForegroundColor Cyan

# Compute address byte (S=1 for commands): 0AAAAAA1
if ($Addr -eq "broadcast") {
    $addrByte = 0xFF
    $queryCmd = "querybc"  # master command for broadcast query
} else {
    $a = [int]$Addr
    if ($a -lt 0 -or $a -gt 63) { throw "Address must be 0-63 or 'broadcast'" }
    $addrByte = ($a -shl 1) -bor 1
    $queryCmd = "query $a"  # master command for addressed query
}

Write-Host "Target: addr byte 0x$($addrByte.ToString('X2')) (short addr $Addr)" -ForegroundColor Cyan

# Open serial ports
$master = New-Object System.IO.Ports.SerialPort $MasterPort, 115200, 'None', 8, 'One'
$master.DtrEnable = $true
$master.RtsEnable = $true
$master.ReadTimeout = 2000
$master.Open()
Start-Sleep -Milliseconds 1000
try { while ($master.BytesToRead -gt 0) { $null = $master.ReadLine() } } catch {}

$slave = $null
try {
    $slave = New-Object System.IO.Ports.SerialPort $SlavePort, 115200, 'None', 8, 'One'
    $slave.ReadTimeout = 500
    $slave.Open()
    Start-Sleep -Milliseconds 300
    try { while ($slave.BytesToRead -gt 0) { Write-Host "  Slave: $($slave.ReadLine())" -ForegroundColor DarkYellow } } catch {}
} catch {
    Write-Host "Warning: Could not open slave port $SlavePort (debug output unavailable)" -ForegroundColor DarkYellow
    $slave = $null
}

function Drain-Slave() {
    if ($null -eq $slave) { return }
    try { while ($slave.BytesToRead -gt 0) { Write-Host "  Slave: $($slave.ReadLine())" -ForegroundColor DarkYellow } } catch {}
}

function Read-MasterResponse($timeoutMs = 2000) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        try {
            while ($master.BytesToRead -gt 0) {
                $line = $master.ReadLine()
                Write-Host "  Master: $line" -ForegroundColor Gray
                if ($line -match "Response:\s*(\d+)") {
                    return [int]$Matches[1]
                }
            }
        } catch {}
        Start-Sleep -Milliseconds 20
    }
    return $null
}

function Send-BootloaderQuery([int]$cmdByte, $desc, $expectAck = $true) {
    # Use master's query/querybc command which calls sendAndReceive (listens for backward frame)
    $cmdStr = "$queryCmd $cmdByte"
    Write-Host "  > $desc ($cmdStr)" -ForegroundColor White
    $master.WriteLine($cmdStr)
    if ($expectAck) {
        $resp = Read-MasterResponse 5000
        Drain-Slave
        if ($resp -eq 1) {
            Write-Host "  ACK" -ForegroundColor Green
            return $true
        } else {
            Write-Host "  FAIL: expected ACK (1), got $resp" -ForegroundColor Red
            return $false
        }
    }
    Start-Sleep -Milliseconds 100
    try { while ($master.BytesToRead -gt 0) { Write-Host "  Master: $($master.ReadLine())" -ForegroundColor Gray } } catch {}
    Drain-Slave
    return $true
}

function Send-Raw($hex, $waitMs = 100) {
    $master.WriteLine("raw $hex")
    Start-Sleep -Milliseconds $waitMs
}

# -- Step 0: Enter bootloader mode via DALI cmd 131 --
if (-not $SkipEntry) {
    Write-Host ""
    Write-Host "=== Entering bootloader (cmd 131 x2) ===" -ForegroundColor Magenta
    $hex = "{0:X2}{1:X2}" -f $addrByte, 131
    Write-Host "  > raw $hex (config repeat x2)" -ForegroundColor White
    Send-Raw $hex 30
    Send-Raw $hex 300
    Drain-Slave
    Write-Host "  Waiting for bootloader to start..." -ForegroundColor Yellow
    Start-Sleep -Milliseconds 500
    Drain-Slave
    # Flush any master output from the reset
    try { while ($master.BytesToRead -gt 0) { $null = $master.ReadLine() } } catch {}
}

# -- Step 1: Erase --
Write-Host ""
Write-Host "=== Erasing user flash ===" -ForegroundColor Magenta
if (-not (Send-BootloaderQuery 0x84 "CMD_ERASE")) {
    Write-Host "Erase failed!" -ForegroundColor Red
    $master.Close(); if ($slave) { $slave.Close() }
    exit 1
}
Write-Host "  Waiting 2s for flash erase to complete..." -ForegroundColor Yellow
Start-Sleep -Milliseconds 2000

# -- Step 2: Send firmware data --
Write-Host ""
Write-Host "=== Uploading $($binData.Length) bytes ===" -ForegroundColor Magenta
$bytesSent = 0
$pageCount = 0
$startTime = Get-Date

for ($i = 0; $i -lt $binData.Length; $i++) {
    # Send CMD_DATA (fire-and-forget, no response expected)
    $cmdHex = "{0:X2}{1:X2}" -f $addrByte, 0x85
    Send-Raw $cmdHex 25

    $bytesSent++
    $isPageBoundary = ($bytesSent % 64 -eq 0)

    if ($isPageBoundary) {
        # Page boundary: use querybc to send data byte AND listen for ACK
        $master.WriteLine("$queryCmd $($binData[$i])")
        Start-Sleep -Milliseconds 50
    } else {
        # Normal: send data byte via raw (no response expected)
        $dataHex = "{0:X2}{1:X2}" -f $addrByte, $binData[$i]
        Send-Raw $dataHex 25
    }

    # ACK expected every 64 bytes (page boundary)
    if ($isPageBoundary) {
        $pageCount++
        $resp = Read-MasterResponse 5000
        if ($resp -ne 1) {
            Write-Host "  FAIL at page $pageCount (byte $bytesSent): expected ACK, got $resp" -ForegroundColor Red
            $master.Close(); if ($slave) { $slave.Close() }
            exit 1
        }
        $pct = [math]::Round($bytesSent / $binData.Length * 100)
        $elapsed = ((Get-Date) - $startTime).TotalSeconds
        $rate = $bytesSent / $elapsed
        $eta = [math]::Round(($binData.Length - $bytesSent) / $rate)
        Write-Host "  Page $pageCount OK  ($bytesSent / $($binData.Length) bytes, ${pct}%, ETA ${eta}s)" -ForegroundColor Green
    }
}

# -- Step 3: Commit (flush partial page) --
Write-Host ""
Write-Host "=== Committing ===" -ForegroundColor Magenta
if (-not (Send-BootloaderQuery 0x86 "CMD_COMMIT")) {
    Write-Host "Commit failed!" -ForegroundColor Red
    $master.Close(); if ($slave) { $slave.Close() }
    exit 1
}

$elapsed = ((Get-Date) - $startTime).TotalSeconds
Write-Host ""
Write-Host "=== Upload complete! ===" -ForegroundColor Green
Write-Host "  $bytesSent bytes in $([math]::Round($elapsed, 1))s ($([math]::Round($bytesSent / $elapsed, 1)) B/s)" -ForegroundColor Green

# -- Step 4: Boot user code --
Write-Host ""
Write-Host "=== Booting user code ===" -ForegroundColor Magenta
Send-BootloaderQuery 0x87 "CMD_BOOT" $false
Start-Sleep -Milliseconds 1000
Drain-Slave

Write-Host ""
Write-Host "Done!" -ForegroundColor Green

$master.Close()
if ($slave) { $slave.Close() }
