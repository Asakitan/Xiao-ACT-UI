<# 
.SYNOPSIS
  打包脚本 — 分别打包 ACT UI 与 Auto Key Server（onefile 模式）
.DESCRIPTION
  1. ACT UI   → dist/XiaoACTUI.exe    (单文件, 无控制台, UAC管理员)
  2. Server   → dist/AutoKeyServer.exe (单文件, 控制台)
#>

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  咲 ACT UI — 打包脚本 (onefile)" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

# ── 前置检查 ──
Write-Host "`n[1/4] 检查 PyInstaller ..." -ForegroundColor Yellow
$pyi = Get-Command pyinstaller -ErrorAction SilentlyContinue
if (-not $pyi) {
    Write-Host "  PyInstaller 未找到，正在安装 ..." -ForegroundColor DarkYellow
    pip install pyinstaller
}
pyinstaller --version

# ── 打包 ACT UI ──
Write-Host "`n[2/4] 打包 ACT UI ..." -ForegroundColor Yellow
Push-Location $root
try {
    pyinstaller XiaoACTUI.spec --noconfirm --clean --distpath "dist"
    if ($LASTEXITCODE -ne 0) { throw "ACT UI 打包失败 (exit $LASTEXITCODE)" }
    Write-Host "  ACT UI 打包完成 → dist\XiaoACTUI.exe" -ForegroundColor Green
} finally {
    Pop-Location
}

# ── 打包 Server ──
Write-Host "`n[3/4] 打包 Auto Key Server ..." -ForegroundColor Yellow
Push-Location (Join-Path $root "server")
try {
    pyinstaller AutoKeyServer.spec --noconfirm --clean --distpath "..\dist" --workpath "..\build\server_build"
    if ($LASTEXITCODE -ne 0) { throw "Server 打包失败 (exit $LASTEXITCODE)" }
    Write-Host "  Server 打包完成 → dist\AutoKeyServer.exe" -ForegroundColor Green
} finally {
    Pop-Location
}

# ── 完成 ──
Write-Host "`n[4/4] 全部打包完成!" -ForegroundColor Green
Write-Host "  ACT UI:  $root\dist\XiaoACTUI.exe"
Write-Host "  Server:  $root\dist\AutoKeyServer.exe"
Write-Host ""
