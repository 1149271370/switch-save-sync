@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

set "PYEXE=%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
set "PYARGS="

if not exist "%PYEXE%" (
  if exist "C:\Windows\py.exe" (
    set "PYEXE=C:\Windows\py.exe"
    set "PYARGS=-3"
  ) else (
    set "PYEXE=python"
  )
)

echo 使用 Python: %PYEXE% %PYARGS%
"%PYEXE%" %PYARGS% -m pip install --upgrade pyinstaller
if errorlevel 1 goto :error

"%PYEXE%" %PYARGS% -m PyInstaller --noconfirm --clean --onefile --windowed --name DaveDiverSaveTransfer dave_save_transfer.pyw
if errorlevel 1 goto :error

echo.
echo 构建完成: dist\DaveDiverSaveTransfer.exe
pause
exit /b 0

:error
echo.
echo 构建失败，请查看上方错误信息。
pause
exit /b 1
