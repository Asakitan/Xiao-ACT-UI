# Maintainer-only packaging helper. Not invoked by any runtime code path.
#
# Produces a self-contained Razorworks/rw_cyber_app.exe for frozen (PyInstaller)
# distributions, so the Cyber Menu subprocess doesn't need a source-tree Python
# to launch. rw_cyber.py automatically prefers this exe over `python rw_cyber_app.py`
# when it exists next to plugin.py.
#
# Usage (run manually from a dev venv that has pyinstaller + pywebview installed):
#   powershell -File build_cyber_app.ps1

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

pyinstaller `
  --onefile `
  --noconsole `
  --name rw_cyber_app `
  --distpath $here `
  --workpath (Join-Path $here "build\rw_cyber_app") `
  --specpath (Join-Path $here "build") `
  --add-data "$here\web\rw_cyber_dashboard.html;web" `
  --add-data "$here\web\rw_cyber_dashboard.css;web" `
  --add-data "$here\web\rw_cyber_dashboard.js;web" `
  (Join-Path $here "rw_cyber_app.py")

Write-Host "Built: $here\rw_cyber_app.exe"
