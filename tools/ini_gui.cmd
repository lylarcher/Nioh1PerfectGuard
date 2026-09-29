@echo off
rem 仁王1 精防 MOD - 配置界面启动器 / launcher (prefers the project .venv)
setlocal
set HERE=%~dp0
if exist "%HERE%..\.venv\Scripts\pythonw.exe" (
  start "" "%HERE%..\.venv\Scripts\pythonw.exe" "%HERE%ini_gui.py" %*
  exit /b 0
)
where pythonw >nul 2>nul
if errorlevel 1 (
  echo 未找到 pythonw.exe。请安装 Python 3 ^(安装时勾选 Add Python to PATH^)，
  echo 或者先在项目根目录执行: python -m venv .venv
  pause
  exit /b 1
)
start "" pythonw "%HERE%ini_gui.py" %*