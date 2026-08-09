# SAO Auto - PowerShell build helper
#
# Parameterised alternative to the .bat scripts.  Handy in CI where the
# calling shell is already PowerShell.
#
# Examples:
#   .\build.ps1 -Preset debug
#   .\build.ps1 -Preset release -Integration
#   .\build.ps1 -Preset hardened -Pack -SignThumbprint ABCD...
#   .\build.ps1 -Preset debug -Clean

[CmdletBinding()]
param(
    [ValidateSet('debug', 'release', 'hardened', 'ninja-debug')]
    [string] $Preset = 'debug',

    [switch] $Clean,
    [switch] $Integration,
    [switch] $Pack,
    [switch] $NoTests,
    [int] $Parallel = 16,

    [string] $SignThumbprint = ''
)

$ErrorActionPreference = 'Stop'

Push-Location $PSScriptRoot
try {
    if (-not $env:VCPKG_ROOT) {
        Write-Error "VCPKG_ROOT environment variable is not set.  See vendor_note.md."
    }

    $presetName = "windows-$Preset"
    $buildDir   = Join-Path $PSScriptRoot "build\$presetName"

    if ($Clean -and (Test-Path $buildDir)) {
        Write-Host "[CLEAN] Removing $buildDir ..." -ForegroundColor Yellow
        Remove-Item -Recurse -Force $buildDir
    }

    Write-Host "[1/3] Configuring $presetName ..." -ForegroundColor Cyan
    & cmake --preset $presetName
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

    Write-Host "[2/3] Building $presetName ..." -ForegroundColor Cyan
    & cmake --build --preset $presetName --parallel $Parallel
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

    if (-not $NoTests) {
        Write-Host "[3/3] Running tests ..." -ForegroundColor Cyan
        if ($Integration -or ($Preset -eq 'hardened')) {
            & ctest --preset $presetName --output-on-failure -j $Parallel
        } else {
            & ctest --preset $presetName --output-on-failure -LE integration -j $Parallel
        }
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "Some tests failed."
        }
    } else {
        Write-Host "[3/3] Skipping tests (-NoTests)." -ForegroundColor DarkGray
    }

    # --- Optional post-processing --------------------------------------------
    $binDir       = Join-Path $buildDir "bin"
    $configuration = switch ($Preset) {
        'release'  { 'RelWithDebInfo' }
        'hardened' { 'Release' }
        default    { 'Debug' }
    }

    function Resolve-BuildExecutable([string] $Name) {
        $configurationPath = Join-Path $binDir "$configuration\$Name"
        if (Test-Path -LiteralPath $configurationPath -PathType Leaf) {
            return $configurationPath
        }
        return Join-Path $binDir $Name
    }

    $launcherPath = Resolve-BuildExecutable 'SaoAuto.exe'
    $signCli      = Join-Path $binDir "tools\sao_sign.exe"
    $packCli      = Resolve-BuildExecutable 'sao_pack.exe'

    if ($SignThumbprint) {
        if (Test-Path $signCli) {
            Write-Host "[sign] Signing $launcherPath ..." -ForegroundColor Cyan
            & $signCli --input $launcherPath --cert $SignThumbprint
            if ($LASTEXITCODE -ne 0) { throw "sao_sign failed" }
        } else {
            Write-Warning "sao_sign.exe not built; cannot post-sign."
        }
    }

    if ($Pack) {
        if (Test-Path $packCli) {
            $version = (Select-String -Path 'CMakeLists.txt' -Pattern 'VERSION\s+([0-9.]+)' | Select-Object -First 1).Matches[0].Groups[1].Value
            $releaseRoot = Join-Path $PSScriptRoot "dist\release\SaoAuto"
            Write-Host "[pack] Assembling onedir tree to $releaseRoot ..." -ForegroundColor Cyan
            & $packCli --input $buildDir --output $releaseRoot --version $version --force
            if ($LASTEXITCODE -ne 0) { throw "sao_pack failed" }
        } else {
            Write-Warning "sao_pack.exe not built; cannot assemble onedir."
        }
    }

    Write-Host ""
    Write-Host "[OK] $Preset build complete." -ForegroundColor Green
    Write-Host "     Launcher: $launcherPath"
} finally {
    Pop-Location
}
