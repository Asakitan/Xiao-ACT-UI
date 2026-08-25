# SAO Auto - PowerShell build helper

[CmdletBinding()]
param(
    [ValidateSet('debug', 'release', 'hardened', 'ninja-debug')]
    [string] $Preset = 'debug',
    [switch] $Clean,
    [switch] $Integration,
    [switch] $Pack,
    [int] $Parallel = 16,
    [string] $SignThumbprint = ''
)

$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    if (-not $env:VCPKG_ROOT) {
        throw "VCPKG_ROOT environment variable is not set. See vendor_note.md."
    }
    $presetName = "windows-$Preset"
    $buildDir = Join-Path $PSScriptRoot "build\$presetName"
    $isShipPreset = $Preset -in @('release', 'hardened')
    if ($Pack -and -not $isShipPreset) {
        throw '-Pack is available only for release or hardened ship presets'
    }
    $stepCount = if ($isShipPreset) { 3 } else { 2 }

    if ($Clean -and (Test-Path $buildDir)) {
        Write-Host "[CLEAN] Removing $buildDir ..." -ForegroundColor Yellow
        Remove-Item -Recurse -Force $buildDir
    }
    Write-Host "[1/$stepCount] Configuring $presetName ..." -ForegroundColor Cyan
    & cmake --preset $presetName
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
    Write-Host "[2/$stepCount] Building $presetName ..." -ForegroundColor Cyan
    & cmake --build --preset $presetName --parallel $Parallel
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

    $binDir = Join-Path $buildDir "bin"
    $configuration = switch ($Preset) {
        'release' { 'RelWithDebInfo' }
        'hardened' { 'Release' }
        default { 'Debug' }
    }
    function Resolve-BuildExecutable([string] $Name) {
        $configurationPath = Join-Path $binDir "$configuration\$Name"
        if (Test-Path -LiteralPath $configurationPath -PathType Leaf) { return $configurationPath }
        return Join-Path $binDir $Name
    }
    $launcherPath = Resolve-BuildExecutable 'SaoAuto.exe'
    $signCli = Resolve-BuildExecutable 'sao_sign.exe'
    if ($SignThumbprint) {
        if ($SignThumbprint -notmatch '^[0-9A-Fa-f]{40}$') {
            throw 'SignThumbprint must contain exactly 40 hexadecimal characters'
        }
        if (-not (Test-Path $signCli)) { throw "requested signing requires sao_sign.exe" }
        Write-Host "[sign] Signing $launcherPath ..." -ForegroundColor Cyan
        & $signCli --input $launcherPath --cert $SignThumbprint
        if ($LASTEXITCODE -ne 0) { throw "sao_sign failed" }
    }
    if ($isShipPreset) {
        Write-Host "[3/$stepCount] Staging and auditing the ship tree ..." -ForegroundColor Cyan
        & cmake --build --preset $presetName --target sao_release_acceptance --parallel $Parallel
        if ($LASTEXITCODE -ne 0) { throw "release acceptance target failed" }
    }

    if ($Pack) {
        $packCli = Resolve-BuildExecutable 'sao_pack.exe'
        if (-not (Test-Path $packCli)) { throw "requested packaging requires sao_pack.exe" }
        $version = (Select-String -Path 'CMakeLists.txt' -Pattern 'VERSION\s+([0-9.]+)' | Select-Object -First 1).Matches[0].Groups[1].Value
        $releaseRoot = Join-Path $PSScriptRoot "dist\release\SaoAuto"
        $shipBin = Join-Path $buildDir 'ship\bin'
        Write-Host "[pack] Assembling from audited ship tree to $releaseRoot ..." -ForegroundColor Cyan
        & $packCli --input $shipBin --output $releaseRoot --version $version --zip --force
        if ($LASTEXITCODE -ne 0) { throw "sao_pack failed" }
        if (-not (Test-Path -LiteralPath (Join-Path $releaseRoot 'manifest.json') -PathType Leaf)) {
            throw 'package manifest was not produced'
        }
        if (-not (Test-Path -LiteralPath "$releaseRoot.zip" -PathType Leaf)) {
            throw 'release ZIP was not produced'
        }
    }
    Write-Host ""
    Write-Host "[OK] $Preset build complete." -ForegroundColor Green
    Write-Host "     Ship tree: $(Join-Path $buildDir 'ship\bin\SaoAuto.exe')"
} finally {
    Pop-Location
}
