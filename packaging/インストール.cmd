@echo off
setlocal
chcp 65001 >nul
title HKPitch のインストール

set "SRC=%~dp0HKPitch"
set "DEST=%ProgramData%\obs-studio\plugins\HKPitch"
if defined HKPITCH_TEST_DEST set "DEST=%HKPITCH_TEST_DEST%"

echo.
echo   HKPitch（OBS 用 声のピッチ・フォルマント フィルタ）をインストールします。
echo.

if not exist "%SRC%\bin\64bit\HKPitch.dll" goto no_files

if defined HKPITCH_TEST_DEST goto copy
tasklist /FI "IMAGENAME eq obs64.exe" 2>nul | find /I "obs64.exe" >nul
if not errorlevel 1 goto obs_running
if not exist "%ProgramFiles%\obs-studio\bin\64bit\obs64.exe" (
  echo   ※ いつもの場所に OBS Studio が見つかりませんでした。
  echo     別の場所に入れている場合は、このまま続けて大丈夫です。
  echo.
)

:copy
if exist "%DEST%" rmdir /s /q "%DEST%"
mkdir "%DEST%" 2>nul
xcopy /E /I /Y /Q "%SRC%" "%DEST%" >nul 2>nul
if not exist "%DEST%\bin\64bit\HKPitch.dll" goto copy_failed

echo   インストールが終わりました。
echo.
echo   次に OBS を起動して、マイクの音声ソースの「フィルタ」を開き、
echo   音声フィルタの「＋」から「HKPitch（声のピッチ・フォルマント）」を追加してください。
echo   くわしくは「説明書.txt」を見てください。
echo.
set "CODE=0"
goto finish

:no_files
echo   [!] 必要なファイルが見つかりません。
echo       ダウンロードした zip を右クリックして「すべて展開」を選び、
echo       展開してできたフォルダの中の「インストール.cmd」をダブルクリックしてください。
echo       （zip を開いたまま中のファイルをダブルクリックすると、このメッセージが出ます）
echo.
set "CODE=1"
goto finish

:obs_running
echo   [!] OBS が起動しています。
echo       OBS を終了してから、もう一度「インストール.cmd」をダブルクリックしてください。
echo.
set "CODE=2"
goto finish

:copy_failed
echo   [!] ファイルをコピーできませんでした。
echo       「インストール.cmd」を右クリックして「管理者として実行」を試してください。
echo       コピー先：%DEST%
echo.
set "CODE=3"
goto finish

:finish
if not defined HKPITCH_TEST_DEST pause
exit /b %CODE%
