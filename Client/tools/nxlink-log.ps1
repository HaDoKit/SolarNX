# Sends a .nro to hbmenu's netloader and shows the app's logWrite() output live, while also
# appending every line - stamped with this PC's HH:mm:ss.fff, the same format as the host's
# SolarNX.log - to <LogDir>/nxlink-<start time>.log.
#
# Why not `nxlink -s ... | Tee-Object`: with its stdout redirected into a pipe, nxlink (a mingw
# program) block-buffers its output, so lines showed up in neither the terminal nor the file until
# kilobytes had accumulated - confirmed live, the log stopped right after startup. Instead this
# script plays nxlink's "-s" server itself: the app's nxlinkStdio() (common/logger/logger.c, DEBUG
# builds) connects back to the PC that sent the .nro on TCP 28771, and every line it sends is
# printed and written immediately.
#
# Windows Firewall may ask once to allow PowerShell to accept connections on that port.

param(
    [Parameter(Mandatory = $true)][string]$SwitchIp,
    [Parameter(Mandatory = $true)][string]$Nro,
    [Parameter(Mandatory = $true)][string]$LogDir,
    [string]$Nxlink = 'C:/devkitPro/tools/bin/nxlink.exe'
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false) # game names etc. are UTF-8

$port = 28771 # NXLINK_CLIENT_PORT - what nxlinkStdio() connects to
New-Item -ItemType Directory -Force $LogDir | Out-Null
$logPath = Join-Path $LogDir ("nxlink-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".log")
Write-Host "nxlink log -> $logPath"

# Listen before sending, so the app can't connect back before anyone is listening.
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Any, $port)
$listener.Start()
$writer = $null
$client = $null
try {
    & $Nxlink -a $SwitchIp $Nro
    if ($LASTEXITCODE -ne 0) {
        Write-Host "nxlink failed (exit code $LASTEXITCODE) - is hbmenu open and idle on the Switch?"
        return
    }

    Write-Host "waiting for the app's log connection on port $port (Ctrl+C to stop)..."
    while (-not $listener.Pending()) { Start-Sleep -Milliseconds 200 } # polling keeps Ctrl+C working
    $client = $listener.AcceptTcpClient()

    $utf8 = [System.Text.UTF8Encoding]::new($false)
    $reader = [System.IO.StreamReader]::new($client.GetStream(), $utf8)
    $writer = [System.IO.StreamWriter]::new($logPath, $true, $utf8)
    $writer.AutoFlush = $true

    while ($null -ne ($line = $reader.ReadLine())) {
        Write-Host $line
        $writer.WriteLine('[' + (Get-Date -Format 'HH:mm:ss.fff') + '] ' + $line)
    }
    Write-Host "the app closed its log connection."
}
finally {
    if ($writer) { $writer.Dispose() }
    if ($client) { $client.Dispose() }
    $listener.Stop()
}
