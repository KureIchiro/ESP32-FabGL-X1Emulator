# Sharp X1 (CZ-800C) / X1turbo Emulator for ESP32

[ [English](#english) | [日本語](#日本語) ]

---

<a id="english"></a>
# English

An emulator for the original Sharp X1 (CZ-800C, not turbo) using ESP32 and the **FabGL** library (Work in Progress). It supports VGA output, PS/2 keyboard and mouse, D88 disk images, the FM sound board (YM2151) and a serial-connected external joystick.

An X1turbo version is a separate sketch in the `X1turbo` folder (see [X1turbo Version](#x1turbo-en)). This X1 version stays as the 15 kHz (640x200) machine for original X1 software.

## 📝 Overview

- **Frame rate**: 60 Hz (262 lines per frame, 200 displayed). VGA output is 640x480 @ 60 Hz (each X1 line is shown twice).
- **Emulated**: Z80 (4 MHz), IPL ROM, sub-CPU (keyboard, game keys, RTC, interrupt vector), i8255 PPI, Z80 CTC, HD46505 CRTC with text / PCG (PCG access synchronised to the CRTC beam, as on the real machine), 640x200 / 320x200 8-colour graphics with palette, priority and simultaneous plane write, AY-3-8910 PSG (2 MHz), MB8877 FDC with D88 images (multi-image files supported) and raw .2D / .FD images, joystick ports.
- **Option boards**: FM sound board CZ-8BS1 (YM2151 4 MHz at 0700h, its CTC at 0704h), mouse board CZ-8BM2 (Z80 SIO at 1F98h, PS/2 mouse; not yet tested with software).
- **X1turbo I/O** (for turbo software that uses only 200-line screens): screen mode 1FD0h (high-speed PCG, two graphic pages), black clip / text key 1FE0h, bank memory 0B00h, kanji ROM (JIS level 1) and kanji VRAM, Z80 DMA, Z80 SIO 1F90h, DIP switch 1FF0h (standard 15 kHz monitor). With `IPLROM.X1T` on the SD card it boots as an X1turbo (see SD Card Structure).
- **Not emulated**: the X1turbo 24 kHz / 400-line display (use the X1turbo version), cassette tape, RS-232C, printer, YM2151 stereo.
- **Audio**: 16384 Hz (FabGL default). PSG and FM are mixed into the 8-bit DAC with the same balance as Common Source Project (PSG channel = 2/3 of an FM channel).

> ⚠️ **Notice Regarding Development & Licenses**
>
> Most of the main source code is built and modified based on AI-generated code. Therefore, the developer does not completely grasp all internal logic and cannot answer detailed questions regarding source code specifications. Thank you for your understanding.
> Please refer to the header comments at the beginning of the source code for license details and agree to them before use.

---

## 🤝 Credits & Acknowledgments

- **Graphics/Audio Engine**: Uses [FabGL](https://github.com/fdivitto/FabGL) by Fabrizio Di Vittorio.
- **Z80 CPU Core**: Modified based on the work by Marat Fayzullin.
- **FM sound (YM2151), video, FDC and the rest**: newly written for this project. The hardware behaviour was studied from the source code of **MAME** and **Common Source Project** (TAKEDA, toshiya) and from X1 technical documents.

### 📜 Core Licenses
The Z80 core follows Marat Fayzullin's **fMSX License**; FabGL follows its own license.

---

## 🛠️ Environment & Build Settings

- **Board Manager**: ESP32 by Espressif Systems (**v2.0.5 Recommended**)
- **Library**: FabGL v1.0.9
- **Board**: `ESP32 Dev Module` (TTGO VGA32)
- **PSRAM**: `"Enabled"` (required). Older ESP32 chips (revision 0–2, found on many TTGO VGA32 boards) have a PSRAM bug (Espressif errata CPU-3.9), and `"Enabled"` makes the compiler add a workaround for it. On chip revision 3 or later you may choose `"Disabled"` instead: the sketch turns PSRAM on by itself and runs about 20% faster without the workaround. On an older chip a `"Disabled"` build stops at startup with a message, because it could occasionally corrupt memory. The upload log shows the revision (e.g. "Chip is ESP32-D0WDQ6 (revision 1)").

---

## 📂 SD Card Structure

```text
/ (SD Root)
└── /X1
    ├── IPL.ROM          : IPL ROM 4KB (required; IPLROM.x1 is also accepted)
    ├── IPLROM.X1T       : X1turbo IPL ROM 32KB (optional). If present, the machine boots with it instead of IPL.ROM (as an X1turbo with a standard 15kHz monitor). Software that needs a plain X1 may not run then; remove this file for it.
    ├── FONT.ROM         : ANK font 2KB, 8x8 x 256 characters (required; FNT0808.x1 is also accepted)
    ├── /BIOS            : the three files above may be placed here instead
    └── <folders>/*.D88  : Disk images (sub-folders are allowed). Raw images *.2D / *.FD (256 bytes x 16 sectors x 2 sides per cylinder, in order) can also be used.
```

---

## 🚀 Boot Menu

| Item | Description |
| :--- | :--- |
| Fd0 / Fd1 | Choose a disk image for each drive. For a file containing several disks, choose which one. If Fd0 is such a file and Fd1 is empty, the next disk goes to Fd1 automatically. The selector opens in the folder of the most recently chosen disk. |
| JoyToKey | ON / OFF: use controller 1 as keys (see Joystick) |
| Joy B1–B3 | The keys for controller buttons 1–3 while JoyToKey is ON. Press Enter, then the key (F6–F12 cancels). Saved in `/X1/JOYKEY.CFG`. |
| Run | Start |

The boot menu and the disk selectors can also be operated with the 1P controller: Up/Down = select (repeats while held), Left/Right = page up/down, button 1 = Enter, button 2 = cancel (ESC).

---

## 🕹️ Operations Guide

- **`[F12]`** : Pause / Resume
- **`[Shift] + [F12]`** : Reset (reboots the ESP32 and returns to the boot menu)
- **While paused `[F10]` / `[F11]`** : Change the disk in FD0 / FD1 (`<NONE>` ejects it)
- **While paused `[Shift] + [F10]` / `[Shift] + [F11]`** : Write FD0 / FD1 back to the SD card (only that disk inside a multi-disk file is overwritten; raw images are written back as raw)
- **`[F8]`** : PSG→SCC on / off (adds SCC preset waveforms played with the PSG's pitch and volume, same as the MSX2 emulator)

### Keys that work differently from their keytop (Japanese 106 keyboard)

| PC keyboard | X1 |
| :--- | :--- |
| End | BREAK (alone: 13h; with Shift: 03h) |
| BackSpace / Delete | DEL (with Shift: INS) |
| Insert | INS |
| Home | HOME (with Shift: CLR) |
| Alt / Muhenkan / Henkan | GRAPH |
| CapsLock | CAPS (lock key; the Caps Lock LED shows it) |
| Katakana/Hiragana | KANA (lock key; the Scroll Lock LED shows it). The kana layout is the same as the kana printed on the 106 keyboard. |
| Ro (`\`) | `_` with Shift (nothing without Shift); ロ in kana mode |
| Numeric keypad Enter | RETURN |
| Cursor keys | Cursor keys, and also the joystick 1 directions (for games that read only the joystick for movement) |
| F1–F5 | X1 F1–F5 (with Shift: F6–F10, as on the X1) |
| F6–F12 | Not sent to the X1. F8 and F10–F12 are emulator operations (above); F6, F7 and F9 do nothing. |

The numeric keypad `=` and `,` keys of the X1 are not on the 106 keyboard.

---

## 🔌 Joystick (External Controller)

Same converter and protocol as the MSX / MarkIII / PC-8801 emulators: serial `IO39`, 115200 bps, 1 byte per port (b7 = port, b6–b4 = buttons 3–1, b3–b0 = right/left/down/up, active low). Ports 1 and 2 are read by the PSG I/O ports (directions + buttons 1–2), as on the real machine.

**JoyToKey** (boot menu, ON / OFF): controller 1 is turned into keys — directions = numeric keypad 1–9 (diagonals = 1 / 3 / 7 / 9), buttons 1–3 = the keys chosen in the boot menu (default SPACE / RETURN / ESC). While it is ON, controller 1 is not connected to joystick port 1. **Not recommended** for games that read keys the normal way: the X1 keyboard only reports the last key pressed, so if you release a button while holding a direction, the direction stops (games that read the game keys with command E3h are not affected).

---

## 🖱️ Mouse

A PS/2 mouse on the mouse port of the board is connected to the emulated mouse board (CZ-8BM2). When software raises RTS of SIO channel B, three bytes (buttons, X movement, Y movement) are received. Not yet tested with real software.

---

## 🔧 Sound Tuning (for builders)

- `ym2151.h` `OPM_FB_CLIP_*`: limits the YM2151 operator-1 feedback to what can be reproduced at 16384 Hz (same as the PC-8801 emulator).
- `X1.ino` `OPM_TREBLE_FIX`: treble correction of the FM output (0 = off).
- YM2151 CSM (each timer A overflow re-keys all channels, as in fmgen; used for speech) is supported.
- `x1.h` `X1_OPM_BOARD`: 0 = no FM sound board.

---

<a id="x1turbo-en"></a>
## ⚡ X1turbo Version (`X1turbo` folder)

A separate sketch (`X1turbo/X1turbo.ino`) emulates an X1turbo with a high-resolution (24 kHz) monitor: 640x400 / 16 colours, 400-line screens, kanji (JIS level 1 / 2) and the other turbo features. See [`X1turbo/README.md`](../X1turbo/README.md).

---

## Disclaimer

IN NO EVENT SHALL FABRIZIO DI VITTORIO, MARAT FAYZULLIN, OR THE DEVELOPER BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE. USE AT YOUR OWN RISK.

---

<a id="日本語"></a>
# 日本語

ESP32 と **FabGL** ライブラリを使った シャープ X1 無印（CZ-800C。turbo ではない）のエミュレータです（開発中）。VGA 出力、PS/2 キーボード・マウス、D88 ディスクイメージ、FM 音源ボード（YM2151）、シリアル接続の外部ジョイスティックに対応しています。

X1turbo 版は `X1turbo` フォルダの別のスケッチです（[X1turbo 版](#x1turbo-ja) を参照）。この X1 版は、無印用のソフトのための 15kHz（640×200）の機種としてそのまま残します。

## 📝 概要

- **フレームレート**: 60Hz（1 フレーム 262 ライン・表示 200 ライン）。VGA の出力は 640×480・60Hz です（X1 の 1 ラインを 2 回表示）。
- **再現しているもの**: Z80（4MHz）、IPL ROM、サブ CPU（キーボード・ゲームキー・時計・割込みベクタ）、i8255 PPI、Z80 CTC、HD46505 CRTC とテキスト・PCG（PCG の読み書きは実機と同じく CRTC のビームの位置に合わせる）、640×200 / 320×200 8 色グラフィック（パレット・優先度・同時書き込み）、AY-3-8910 PSG（2MHz）、MB8877 FDC と D88 イメージ（1 ファイルに複数枚入った D88 も可）・ベタの .2D / .FD イメージ、ジョイスティック端子。
- **拡張ボード**: FM 音源ボード CZ-8BS1（YM2151 4MHz、0700h。CTC は 0704h）、マウスボード CZ-8BM2（Z80 SIO 1F98h、PS/2 マウス。ソフトでの確認はまだ）。
- **X1turbo の I/O**（200 ラインの画面だけを使う turbo 用のソフトのため）: 画面モード 1FD0h（高速 PCG、グラフィック 2 ページ）、ブラッククリップ・テキストキー 1FE0h、バンクメモリ 0B00h、漢字 ROM（JIS 第 1 水準）と漢字 VRAM、Z80 DMA、Z80 SIO 1F90h、DIP スイッチ 1FF0h（標準の 15kHz モニタ）。SD カードに `IPLROM.X1T` があれば X1turbo として起動します（「SD カードの構成」を参照）。
- **再現していないもの**: X1turbo の 24kHz・400 ラインの画面（X1turbo 版を使ってください）、カセットテープ、RS-232C、プリンタ、YM2151 のステレオ。
- **サウンド**: 16384Hz（FabGL の標準）。PSG と FM を混ぜて 8bit DAC に出力します。音量の比は Common Source Project と同じです（PSG 1ch = FM 1ch の 2/3）。

> ⚠️ **開発・ライセンスに関するご注意**
>
> メインソースコードの大部分は AI 生成コードをベースに構築・改変しているため、開発者もすべてのロジックを完全に把握しているわけではありません。そのため、ソースコードの詳細な仕様や内部ロジックに関する質問にはお答えできかねます。あらかじめご了承ください。
> ライセンスの詳細な条件はソースコード冒頭のヘッダコメントを参照し、同意の上でご利用ください。

---

## 🤝 クレジットと謝辞

- **グラフィック/オーディオエンジン**: Fabrizio Di Vittorio 氏による [FabGL](https://github.com/fdivitto/FabGL) を使用。
- **Z80 CPU コア**: Marat Fayzullin 氏のコードをベースに改変。
- **FM 音源（YM2151）、画面、FDC など**: 本プロジェクトで新規に作成。実機の動作は **MAME** と **Common Source Project**（TAKEDA, toshiya 氏）のソースコード、X1 の技術資料で調べました。

### 📜 各コアのライセンスについて
Z80 コアは Marat Fayzullin 氏の **fMSX License**、FabGL はそのライセンスに従います。

---

## 🛠️ 開発環境とビルド設定

- **ボードマネージャ**: ESP32 by Espressif Systems（**v2.0.5 推奨**）
- **ライブラリ**: FabGL v1.0.9
- **ボード**: `ESP32 Dev Module`（TTGO VGA32）
- **PSRAM**: `"Enabled"`（必須）。古い ESP32（リビジョン 0〜2。TTGO VGA32 にも多い）には PSRAM の不具合（Espressif errata CPU-3.9）があり、`"Enabled"` にするとコンパイラがその回避策を入れます。チップがリビジョン 3 以降なら `"Disabled"` でもよく、スケッチが起動時に PSRAM を有効にし、回避策が入らないぶん約 2 割速くなります。古いチップで `"Disabled"` にしたものは、まれにメモリが壊れるため、起動時に理由を表示して止まります。リビジョンは書き込みのときのログ（「Chip is ESP32-D0WDQ6 (revision 1)」など）で分かります。

---

## 📂 SD カードの構成

```text
/ (SD ルート)
└── /X1
    ├── IPL.ROM          : IPL ROM 4KB（必須。IPLROM.x1 という名前でもよい）
    ├── IPLROM.X1T       : X1turbo の IPL ROM 32KB（なくてもよい）。あれば IPL.ROM の代わりにこれで起動する（標準の 15kHz モニタをつないだ X1turbo として）。そのときは無印の X1 でないと動かないソフトが動かないことがあるので、そのソフトはこのファイルを外して起動する
    ├── FONT.ROM         : ANK フォント 2KB（8×8・256 文字。必須。FNT0808.x1 という名前でもよい）
    ├── /BIOS            : 上の 3 つはこのフォルダに置いてもよい
    └── <フォルダ>/*.D88 : ディスクイメージ（フォルダ分けしてよい）。ベタのイメージ *.2D / *.FD（1 セクタ 256 バイト × 16 セクタ × 2 面を順に並べたもの）も使える
```

---

## 🚀 起動メニュー

| 項目 | 内容 |
| :--- | :--- |
| Fd0 / Fd1 | 各ドライブのディスクイメージを選ぶ。複数枚入ったファイルは何枚目かを選ぶ。Fd0 に複数枚入ったファイルを選び Fd1 が空なら、次の 1 枚を自動で Fd1 に入れる。選択画面は、最後に選んだディスクのフォルダから開く。 |
| JoyToKey | ON / OFF: コントローラ 1 をキーとして使う（「ジョイスティック」を参照） |
| Joy B1〜B3 | JoyToKey が ON のときの、コントローラのボタン 1〜3 のキー。Enter を押してからキーを押す（F6〜F12 で取り消し）。`/X1/JOYKEY.CFG` に保存。 |
| Run | 起動 |

起動メニューとディスクの選択画面は、1P のコントローラでも操作できます：↑↓ = 選択（押し続けると連続）、←→ = 1画面送り、ボタン 1 = 決定（Enter）、ボタン 2 = 取り消し（ESC）。

---

## 🕹️ 操作方法

- **`[F12]`** : 一時停止 / 再開
- **`[Shift] + [F12]`** : リセット（ESP32 ごと再起動して起動メニューへ）
- **一時停止中に `[F10]` / `[F11]`** : FD0 / FD1 のディスクを入れ替え（`<NONE>` で取り出し）
- **一時停止中に `[Shift] + [F10]` / `[Shift] + [F11]`** : FD0 / FD1 の内容を SD へ書き戻し（複数枚入ったファイルは、そのディスクの部分だけ上書き。ベタのイメージはベタのまま書き戻す）
- **`[F8]`** : PSG→SCC の入/切（PSG の音程・音量で SCC のプリセット波形を重ねて鳴らす。MSX2 版と同じ）

### 印字と機能が違うキー（日本語 106 キーボード）

| PC のキーボード | X1 |
| :--- | :--- |
| End | BREAK（BREAK だけでは 13h、Shift + BREAK で 03h） |
| BackSpace / Delete | DEL（Shift で INS） |
| Insert | INS |
| Home | HOME（Shift で CLR） |
| Alt・無変換・変換 | GRAPH |
| CapsLock | CAPS（ロック式。Caps Lock のランプが連動） |
| カタカナひらがな | カナ（ロック式。Scroll Lock のランプが連動）。カナの配列は 106 キーボードのかなの印字と同じです。 |
| ろ（`\`） | Shift で `_`（Shift なしでは何も入りません）。カナのときは ロ |
| テンキーの Enter | RETURN |
| カーソルキー | カーソルキー。ジョイスティック 1 の方向にもなります（移動をジョイスティックでしか読まないソフト用） |
| F1〜F5 | X1 の F1〜F5（実機と同じく Shift で F6〜F10） |
| F6〜F12 | X1 には送りません。F8・F10〜F12 はエミュレータの操作（上の一覧）、F6・F7・F9 は何もしません。 |

X1 のテンキーの `=` と `,` は 106 キーボードにありません。

---

## 🔌 ジョイスティック（外部コントローラ）

MSX・マーク III・PC-8801 版と同じ変換器・同じ通信仕様です：シリアル `IO39`、115200bps、ポートごとに 1 バイト（b7 = ポート、b6〜b4 = ボタン 3〜1、b3〜b0 = 右/左/下/上、0 = 押下）。実機と同じく、ポート 1・2 を PSG の I/O ポートから読みます（方向＋ボタン 1〜2）。

**JoyToKey**（起動メニューで ON / OFF）: コントローラ 1 をキーにします。方向 = テンキーの 1〜9（斜めは 1・3・7・9）、ボタン 1〜3 = 起動メニューで決めたキー（初めは SPACE / RETURN / ESC）。ON の間、コントローラ 1 はジョイスティック端子 1 にはつながりません。キーを通常の読み方で読むゲームでは、方向を押したままボタンを離すと方向が途切れるため**非推奨**です（X1 のキーボードは最後に押したキーしか送らないため。ゲーム用キーの命令 E3h で読むゲームは問題ありません）。

---

## 🖱️ マウス

ボードのマウス端子につないだ PS/2 マウスを、マウスボード（CZ-8BM2）として扱います。ソフトが SIO のチャンネル B の RTS を立てると、3 バイト（ボタン・X の移動量・Y の移動量）が届きます。実際のソフトでの確認はまだです。

---

## 🔧 音の調整（ビルドする人向け）

- `ym2151.h` の `OPM_FB_CLIP_*`: YM2151 のオペレータ 1 のフィードバックを、16384Hz で再現できる強さまでに制限する（PC-8801 版と同じ）。
- `X1.ino` の `OPM_TREBLE_FIX`: FM の高音の補正（0 で補正なし）。
- YM2151 の CSM（タイマー A があふれるたびに全チャンネルをキーオンし直す。fmgen と同じ。音声合成に使われる）に対応しています。
- `x1.h` の `X1_OPM_BOARD`: 0 で FM 音源ボードなし。

---

<a id="x1turbo-ja"></a>
## ⚡ X1turbo 版（`X1turbo` フォルダ）

高解像度（24kHz）モニタをつないだ X1turbo として動く、別のスケッチ（`X1turbo/X1turbo.ino`）です。640×400・16 色、400 ラインの画面、漢字（JIS 第 1・第 2 水準）などの turbo の機能を再現しています。[`X1turbo/README.md`](../X1turbo/README.md) を参照してください。

---

## 免責事項

本ソフトウェアの使用によって生じたいかなる損害についても、FABRIZIO DI VITTORIO 氏、MARAT FAYZULLIN 氏、および開発者は一切の責任を負いません。自己責任でご利用ください。
