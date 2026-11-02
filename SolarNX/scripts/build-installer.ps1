# Non-interactive installer build: dotnet publish -> Inno Setup compile.
# Produces Setup\SolarNX-Setup.exe. Not part of the normal F5 dev loop
# (see ../DEVELOPMENT.md) - this is release packaging only.
#
# -Version overrides the version installer.iss reads from ../VERSION (the
# release workflow passes the git tag); omit it for a local build.
param(
    [string]$Version
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$csproj = Join-Path $projectRoot "SolarNX.csproj"
$publishDir = Join-Path $projectRoot "publish\win-x64"
$installerIss = Join-Path $projectRoot "installer.iss"

Write-Host "Publishing win-x64 self-contained build..."
& dotnet publish $csproj -c Release -r win-x64 --self-contained true `
    -p:PublishSingleFile=false -o $publishDir
if ($LASTEXITCODE -ne 0) {
    Write-Error "dotnet publish failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

$iscc = Get-Command "ISCC.exe" -ErrorAction SilentlyContinue
if (-not $iscc) {
    $candidates = @(
        "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
        "C:\Program Files\Inno Setup 6\ISCC.exe"
    )
    $iscc = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) {
        Write-Error "ISCC.exe (Inno Setup Compiler) not found on PATH or in the default install locations. Install Inno Setup 6: https://jrsoftware.org/isdl.php"
        exit 1
    }
}
else {
    $iscc = $iscc.Source
}

Write-Host "Compiling installer via $iscc..."
if ($Version) {
    & $iscc "/DMyAppVersion=$Version" $installerIss
}
else {
    & $iscc $installerIss
}
if ($LASTEXITCODE -ne 0) {
    Write-Error "ISCC failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

Write-Host "Done: $projectRoot\Setup\SolarNX-Setup.exe"
