# Builds SolarNX-Forwarder.nsp: an installable title that puts an SolarNX tile on the Switch HOME
# menu and, when started, launches the .nro from the microSD card (so updating the app = copying a
# new .nro, the tile stays).
#
# Uses nton (python -m pip install nton) with YOUR console's keys at %USERPROFILE%\.switch\prod.keys
# (dumped from your own console; never commit or share them - *.keys is gitignored). Install the
# resulting .nsp on the console with DBI (or similar) - this script only builds the file.
#
# The Title ID is pinned (the same one the earlier hand-built forwarder used), so installing a
# rebuild updates the existing tile instead of adding a second one.

param(
    # The .nro the tile launches - its path ON THE SD CARD. Copy the built .nro there.
    [string]$SdPath = '/switch/SolarNX/SolarNX-Switch.nro',
    [string]$Nro = (Join-Path (Split-Path -Parent $PSScriptRoot) 'SolarNX-Switch.nro'),
    [string]$Icon = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'SolarNX\Assets\icon.png'),
    [string]$Version = '1.0.0',
    [string]$Publisher = 'HaDoK',
    [string]$TitleId = '01E7E36C3EA40000',
    [string]$Out = (Join-Path (Split-Path -Parent $PSScriptRoot) 'SolarNX-Forwarder.nsp')
)

# Not 'Stop': nton logs its normal progress to stderr, which Windows PowerShell 5.1 would turn into a
# terminating NativeCommandError. Failures are caught by the explicit checks/throws below instead.
$ErrorActionPreference = 'Continue'

$keys = Join-Path $env:USERPROFILE '.switch\prod.keys'
if (-not (Test-Path $keys)) { throw "prod.keys not found at $keys - dump them from your own console (Lockpick_RCM)." }
if (-not (Test-Path $Nro)) { throw ".nro not found at $Nro - build the client first (make)." }
& python -m pip show nton *> $null
if ($LASTEXITCODE -ne 0) { throw "nton is not installed - run: python -m pip install nton" }

# nton always saves to <Desktop>\NTON\ as "<name> v<version> by <publisher> [<titleid>].nsp" - pick our
# file up from there (only one written by this run) and move it next to the .nro.
$ntonDir = Join-Path ([Environment]::GetFolderPath('Desktop')) 'NTON'
$expectedName = "SolarNX v$Version by $Publisher [$($TitleId.ToLower())].nsp"
$expectedPath = Join-Path $ntonDir $expectedName
# nton overwrites a same-named file without asking - park any existing one (e.g. a forwarder built by
# hand earlier) and put it back afterwards. Learned the hard way: the first run of this script
# replaced such a file.
$parked = "$expectedPath.solarnx-parked"
if (Test-Path -LiteralPath $expectedPath) { Move-Item -LiteralPath $expectedPath -Destination $parked -Force -ErrorAction Stop }
$started = (Get-Date).AddSeconds(-2)
$ntonArgs = @('-m', 'nton', 'build', $Nro, '--sdmc', $SdPath, '--name', 'SolarNX', '--publisher', $Publisher,
              '--version', $Version, '--id', $TitleId)
if (Test-Path $Icon) { $ntonArgs += @('--icon', $Icon) }
try {
    & python @ntonArgs
    if ($LASTEXITCODE -ne 0) { throw "nton failed (exit code $LASTEXITCODE)" }

    $nsp = Get-Item -LiteralPath $expectedPath -ErrorAction SilentlyContinue
    if (-not $nsp -or $nsp.LastWriteTime -lt $started) { throw "nton finished but $expectedName was not written in $ntonDir" }
    Move-Item -LiteralPath $nsp.FullName -Destination $Out -Force -ErrorAction Stop
}
finally {
    if (Test-Path -LiteralPath $parked) { Move-Item -LiteralPath $parked -Destination $expectedPath -Force }
}
# Leave the NTON folder alone if it holds anything else (e.g. forwarders you built yourself).
if (-not (Get-ChildItem -LiteralPath $ntonDir -ErrorAction SilentlyContinue)) { Remove-Item -LiteralPath $ntonDir -ErrorAction SilentlyContinue }

Write-Host ""
Write-Host "Built $Out (Title ID $TitleId)."
Write-Host "1. Copy $Nro to the SD card as sdmc:$SdPath"
Write-Host "2. Install $(Split-Path -Leaf $Out) on the console (DBI etc.) - the SolarNX tile appears on the HOME menu."
