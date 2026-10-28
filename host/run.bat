@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"

rem 优先使用 Windows Python Launcher，避免 PATH 中的其他 Python 抢占。
where pyw.exe >nul 2>nul
if not errorlevel 1 (
    start "" pyw.exe -3 "%~dp0main.py"
    exit /b 0
)

where py.exe >nul 2>nul
if not errorlevel 1 (
    py.exe -3 "%~dp0main.py"
    exit /b
)

where pythonw.exe >nul 2>nul
if not errorlevel 1 (
    start "" pythonw.exe "%~dp0main.py"
    exit /b 0
)

where python.exe >nul 2>nul
if errorlevel 1 goto no_python

python.exe "%~dp0main.py"
exit /b %errorlevel%

:no_python
echo [错误] 未找到 Python。请先安装 Python 3.10 或更高版本，并勾选 Tcl/Tk。
pause
exit /b 1
