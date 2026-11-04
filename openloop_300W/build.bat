@echo off
chcp 65001 >nul
echo ================================
echo  300W F28P55x 开环 + int8 AI 编译
echo ================================
cd /d "%~dp0"
python build_fw_int8.py
if errorlevel 1 (
  echo.
  echo 编译失败，请看上面的错误
) else (
  echo.
  echo 编译成功: build\openloop_int8.out
  echo 烧录: DSLite.exe flash -c targetConfigs\TMS320F28P550SJ9_LaunchPad.ccxml -f build\openloop_int8.out -u
)
pause
