@echo off
cd /d "%~dp0"
where pythonw >nul 2>nul
if %errorlevel%==0 (
    start "" pythonw "%~dp0wristband_monitor.py"
) else (
    python "%~dp0wristband_monitor.py"
    if errorlevel 1 pause
)
