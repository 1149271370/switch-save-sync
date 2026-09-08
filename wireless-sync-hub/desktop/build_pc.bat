@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

set "PYEXE=%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
if not exist "%PYEXE%" set "PYEXE=py -3"

if exist "%PYEXE%" (
  "%PYEXE%" -m pip install --upgrade -r requirements.txt
  "%PYEXE%" -m PyInstaller --noconfirm --clean --onefile --windowed --name SwitchSaveSyncHub sync_hub.py
) else (
  %PYEXE% -m pip install --upgrade -r requirements.txt
  %PYEXE% -m PyInstaller --noconfirm --clean --onefile --windowed --name SwitchSaveSyncHub sync_hub.py
)

echo Build output: dist\SwitchSaveSyncHub.exe
pause
