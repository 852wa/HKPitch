@echo off
chcp 65001 >nul
rem 開発用：ビルド済みのプラグインをこの PC の OBS に入れる（OBS を終了してから実行。配布用は packaging/ のほう）
tasklist /FI "IMAGENAME eq obs64.exe" | find /I "obs64.exe" >nul
if not errorlevel 1 (
  echo OBS が起動中です。OBS を終了してから、もう一度実行してください。
  pause
  exit /b 1
)
set "DEST=%ProgramData%\obs-studio\plugins\HKPitch"
mkdir "%DEST%\bin\64bit" 2>nul
mkdir "%DEST%\data" 2>nul
copy /Y "%~dp0..\build\Release\HKPitch.dll" "%DEST%\bin\64bit\" >nul || goto fail
xcopy /E /I /Y /Q "%~dp0..\data" "%DEST%\data" >nul || goto fail
echo HKPitch を OBS に入れました。OBS を起動してください。
pause
exit /b 0
:fail
echo コピーに失敗しました。
pause
exit /b 1
