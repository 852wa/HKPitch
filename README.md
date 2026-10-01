# HKPitch

OBS Studio 用の音声フィルタです。声の **高さ（ピッチ）** と **響き・声質（フォルマント）** を、別々になめらかに変えられます。

> **このプラグインは AI を使って開発しましたが、AI ボイスチェンジャーではありません。**
> 声の高さや響きを、昔からある一般的なピッチシフト（信号処理）で変えるものです。
> AI モデルや学習データは使っていません。同じ声を入れれば毎回同じ結果になり、GPU も不要です。

*English: HKPitch is a pitch & formant shifter filter for OBS Studio. It was developed with the help of AI, but it is **not** an AI voice changer — it uses classic signal processing (YIN pitch detection + PSOLA), with no machine-learning model or training data.*

## インストール（かんたん）

1. OBS を終了する
2. [Releases](https://github.com/852wa/HKPitch/releases) から `HKPitch-<版>-windows-x64.zip` をダウンロードし、右クリック →「すべて展開」
3. 展開したフォルダの `インストール.cmd` をダブルクリック
4. OBS を起動し、マイクの「フィルタ」→ 音声フィルタの「＋」→「HKPitch（声のピッチ・フォルマント）」

管理者の権限やコマンド入力は要りません。中身は `HKPitch` フォルダを `C:\ProgramData\obs-studio\plugins\` にコピーしているだけなので、手でコピーしても同じです。くわしい使い方は zip の中の `説明書.txt` にあります。

動作環境：Windows 10 / 11（64 ビット）、OBS Studio 32 で動作確認。

## できること

| 項目 | 内容 |
|---|---|
| ぱきっと（最初はオン） | 3 バンドイコライザで中域（800Hz〜5kHz）を下げてから処理する。輪郭のはっきりした音になる |
| プリセット | 子ども / 女の子 / よそいき / 元に戻す / 男の子 / 男性 / 低い男性 |
| ピッチ | 声の高さ（半音、±24）。フォルマント 0 なら声質はそのままで高さだけ変わる |
| フォルマント | 高さを変えずに響きだけを動かす（半音、±12）。＋で小柄・若く、−で大柄・太く |
| 処理モード | 高品質（おすすめ・遅延 約 32ms）／バランス（約 24ms）／低遅延（約 20ms） |
| 加工した声の割合・出力の音量 | ピッチを上げると少し音量が下がるので、出力の音量で補える |

- ひとりの声専用です（BGM や楽器が混ざった音には向きません）。
- スライダーを動かすと、約 40ms かけてなめらかに新しい高さへ移ります。
- CPU の負担は 1 コアの 1% 未満です。左右が同じ音（普通のマイク）なら 1 チャンネル分だけ処理します。

## しくみ

- **高さを測る**：YIN 法。いちばん新しい音から比べるので、声の出だしにすぐ気づける。息まじりの声や声の終わりでも判定が外れにくいよう、少しのあいだ有声のまま保つ。
- **高さを変える**：PSOLA。1 周期ごとに目印を置き（前の周期との相関で位置をそろえる）、前後の目印までを左右非対称の窓で切り出して、新しいピッチの間隔で並べ直す。窓がぴったり 1 に足し合わさるので、ピッチ 0・フォルマント 0 なら元の声がそのまま出る。
- **響きを変える**：切り出した 1 周期ぶんを伸び縮みさせる。無声音（子音）は並べ替えずにそのまま通す。
- 音を周波数に分解しないので、声の前後に音がにじまず、二重に聞こえにくい。
- `src/voice-shifter.cpp` は比較用の周波数分解方式（位相ロック付きフェーズボコーダ + True Envelope）で、プラグインには入っていません。

## ビルド

必要なもの：Visual Studio 2022、CMake 3.22 以上、OBS Studio 32 のソース（`libobs` のヘッダ用）と `obs.lib`。

`obs.lib` は、OBS の公式の手順でビルドするか、インストール済みの `obs.dll` の関数一覧から `.def` ファイルを作り、`lib /def:obs.def /machine:x64 /out:obs.lib` で作れます。場所は `local.cmake`（リポジトリには入れないファイル）に書きます。

```cmake
# local.cmake
set(OBS_SOURCE_DIR "C:/path/to/obs-studio" CACHE PATH "")
set(OBS_LIBRARY "C:/path/to/obs.lib" CACHE FILEPATH "")
set(HKPITCH_INSTALL_TO_OBS ON CACHE BOOL "")   # ビルドのたびに自分の OBS へコピーする
```

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

```bash
cmake --build build --config Release
```

OBS の起動中は自動コピーに失敗するので、OBS を終了して `tools/install-dev.cmd` を実行してください。

配布用 zip は次で `dist/` にできます。

```bash
pwsh tools/make-release.ps1
```

## テスト

OBS なしで音声処理だけを確かめる道具です。ピッチの正確さ、倍音の濁り、フォルマントの位置、揺れのある声での自然さ、声の前後のにじみ、声の出だし、ピッチを動かしたときのなめらかさ、ぱきっと、処理の重さを数値で確認します。

```bash
build/Release/hkpitch-offline.exe
```

WAV を加工して聞き比べる（ピッチ・フォルマントは半音。最後に `sweep` を付けるとピッチをゆっくり上下させる）：

```bash
build/Release/hkpitch-offline.exe render 入力.wav 出力.wav 4 2
```

## 作者・ライセンス

Copyright (C) 2026 hakoniwa

[GPL-2.0-or-later](LICENSE)（OBS Studio と同じ）
