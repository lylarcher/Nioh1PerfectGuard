@echo off
rem 重新打包配置界面 exe（需要 .venv 里已安装 pyinstaller）
setlocal
cd /d "%~dp0.."
if not exist .venv\Scripts\python.exe ( echo 先执行: python -m venv .venv ^&^& .venv\Scripts\python -m pip install pyinstaller & pause & exit /b 1 )
.venv\Scripts\python.exe -m PyInstaller --noconfirm --onefile --windowed --name Nioh1PerfectGuard-Config --distpath tools\gui_dist --workpath _work\pyi --specpath _work\pyi tools\ini_gui.py
echo 输出: tools\gui_dist\Nioh1PerfectGuard-Config.exe
pause