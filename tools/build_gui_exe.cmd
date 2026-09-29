@echo off
rem 仁王1 精防 MOD - 配置界面 exe 构建 / build the config GUI exe
rem 实际逻辑在 tools\build_gui.ps1（单一来源），这里只是双击入口。
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_gui.ps1" %*
pause