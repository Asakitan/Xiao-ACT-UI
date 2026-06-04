net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Readmin...
    powershell -Command "Start-Process '%~f0' -Verb runAs"
    exit /b
)

cd /d "%~dp0"
python main.py
pause