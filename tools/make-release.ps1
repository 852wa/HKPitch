# 配布用 zip を作る（PowerShell 7 で実行）
#   pwsh tools/make-release.ps1
# できるもの：dist/HKPitch-<版>-windows-x64.zip
param(
    [string]$SourceUrl = "https://github.com/852wa/HKPitch"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$version = (Select-String -Path "$root/CMakeLists.txt" -Pattern 'project\(HKPitch VERSION ([0-9.]+)').Matches[0].Groups[1].Value
if (-not $SourceUrl) {
    Write-Warning "ソースコードの公開場所（-SourceUrl）が指定されていません。説明書には「（公開先を準備中）」と入ります。GPL では配布物からソースにたどれる必要があるので、公開前に指定してください。"
    $SourceUrl = "（公開先を準備中）"
}

# プラグインをビルド（OBS への自動コピーはしない）
cmake --build "$root/build" --config Release --target HKPitch | Out-Host
if ($LASTEXITCODE -ne 0) { throw "ビルドに失敗しました" }

$name = "HKPitch-$version-windows-x64"
$stage = "$root/dist/$name"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force "$stage/HKPitch/bin/64bit" | Out-Null
Copy-Item "$root/build/Release/HKPitch.dll" "$stage/HKPitch/bin/64bit/"
Copy-Item -Recurse "$root/data" "$stage/HKPitch/data"

# バッチファイルは改行を CRLF・BOM なしに（cmd が正しく読めるように）
foreach ($f in "インストール.cmd", "アンインストール.cmd") {
    $text = (Get-Content -Raw -Encoding utf8 "$root/packaging/$f") -replace "`r?`n", "`r`n"
    [IO.File]::WriteAllText("$stage/$f", $text, [Text.UTF8Encoding]::new($false))
}
# 説明書はメモ帳で文字化けしないよう BOM 付き UTF-8・CRLF に
$doc = (Get-Content -Raw -Encoding utf8 "$root/packaging/説明書.txt").Replace("{{VERSION}}", $version).Replace("{{SOURCE_URL}}", $SourceUrl) -replace "`r?`n", "`r`n"
[IO.File]::WriteAllText("$stage/説明書.txt", $doc, [Text.UTF8Encoding]::new($true))
$lic = (Get-Content -Raw -Encoding utf8 "$root/LICENSE") -replace "`r?`n", "`r`n"
[IO.File]::WriteAllText("$stage/LICENSE.txt", $lic, [Text.UTF8Encoding]::new($false))

$zip = "$root/dist/$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip
Write-Host "できました: $zip"
