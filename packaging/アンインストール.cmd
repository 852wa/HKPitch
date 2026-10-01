@echo off
setlocal
chcp 65001 >nul
title HKPitch のアンインストール

set "DEST=%ProgramData%\obs-studio\plugins\HKPitch"
if defined HKPITCH_TEST_DEST set "DEST=%HKPITCH_TEST_DEST%"

echo.
echo   HKPitch を OBS から取りのぞきます。
echo.

if not exist "%DEST%" goto not_installed

if defined HKPITCH_TEST_DEST goto remove
tasklist /FI "IMAGENAME eq obs64.exe" 2>nul | find /I "obs64.exe" >nul
if not errorlevel 1 goto obs_running

:remove
rmdir /s /q "%DEST%"
if exist "%DEST%" goto remove_failed
echo   取りのぞきました。
echo   OBS のシーンに付けていた HKPitch のフィルタは、OBS 側で削除してください。
echo.
set "CODE=0"
goto finish

:not_installed
echo   HKPitch は入っていませんでした。
echo.
set "CODE=0"
goto finish

:obs_running
echo   [!] OBS が起動しています。OBS を終了してから、もう一度ダブルクリックしてください。
echo.
set "CODE=2"
goto finish

:remove_failed
echo   [!] 取りのぞけませんでした。右クリックして「管理者として実行」を試してください。
echo       場所：%DEST%
echo.
set "CODE=3"
goto finish

:finish
if not defined HKPITCH_TEST_DEST pause
exit /b %CODE%
