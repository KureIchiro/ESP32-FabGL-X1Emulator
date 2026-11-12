# Sharp X1turbo Emulator for ESP32

[ [English](#english) | [日本語](#日本語) ]

---

<a id="english"></a>
# English

An emulator for the Sharp X1turbo with a high-resolution (24 kHz) monitor, using ESP32 and the **FabGL** library (Work in Progress). It supports VGA output, PS/2 keyboard and mouse, D88 / raw disk images, the FM sound board (YM2151) and a serial-connected external joystick.

This is a separate sketch from the original X1 version (`X1` folder). The X1 version is the 15 kHz (640x200) machine for original X1 software; this version is fixed to the 24 kHz (640x400) display.

## 📝 Overview

- **Display**: 640x400, 16 colours (FabGL VGA16Controller). 400-line (24 kHz) screens are shown line for line; 200-line screens show each line twice. Colours 0–7 are the text colours and 8–15 the graphics colours, so palette changes never redraw the screen.
- **Frame rate**: 55.5 Hz, the same as an X1turbo at 24 kHz. Each frame starts with a VGA frame, so the picture does not tear. Lines per frame and CPU cycles per line follow the CRTC settings as on the real machine (they decide when PCG is written during the blanking period). Original X1 software that sets the CRTC for 15 kHz runs about 10 % slower; use the X1 version for it. When a frame takes too long, the next frame is not drawn (the emulation keeps its speed).
- **Emulated**: Z80 (4 MHz), 32 KB IPL ROM, sub-CPU (keyboard, game keys, RTC, interrupt vector), i8255 PPI, Z80 CTC, HD46505 CRTC with text / PCG (PCG access synchronised to the CRTC beam), 640x200 / 320x200 / 400-line 8-colour graphics with palette, priority and simultaneous plane write, AY-3-8910 PSG (2 MHz), MB8877 FDC with D88 images (multi-image files supported) and raw .2D / .FD images, joystick ports.
- **Turbo features** (behaviour taken from Common Source Project and MAME): screen mode register 1FD0h (high-speed PCG, two graphic pages for display / CPU access, 400-line graphics, ANK 8x16 font), black clip / text key 1FE0h, kanji VRAM (3800h) and kanji display (JIS level 1 / 2, 16-line user-defined characters), kanji ROM read ports (0E80h–0E82h), Z80 DMA (1F80h, used with the FDC), bank memory 0B00h (32 KB x 16), Z80 CTC 1FA0h, Z80 SIO 1F90h, turbo keyboard (changes of CTRL / SHIFT / KANA / CAPS / GRAPH are also sent), DIP switch 1FF0h (fixed: high-resolution monitor, 5-inch 2D drive).
- **Option boards**: FM sound board CZ-8BS1 (YM2151 4 MHz at 0700h, its own CTC at 0704h; CSM speech supported), mouse board CZ-8BM2 (Z80 SIO at 1F98h, PS/2 mouse; not yet tested with software).
- **Not emulated**: X1turboZ features (4096 colours etc.), VRAM wait states, kanji underline, 2HD disks, cassette tape, RS-232C, printer, YM2151 stereo.
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
- **PSRAM** (ON = `"Enabled"`, OFF = `"Disabled"`): With PSRAM ON, a software workaround for the chip errata (Espressif errata CPU-3.9) is built in, and the speed drops by about 20%. With PSRAM OFF, the sketch enables PSRAM by itself and the software workaround is not built in. However, this causes failures unless the ESP32 is chip revision 3 or later, so on any other revision a PSRAM OFF build is made not to start. This project uses frame skipping to keep the feel close to the real machine; turning PSRAM OFF brings it closer to 60/55 FPS.
- **Switching machines with EmuLauncher** (optional): copy `emu_switch.h` and `partitions.csv` from the `EmuLauncher` folder into this sketch folder. The boot menu then gets **Next** (the machine to start next), and the flash is laid out as in `partitions.csv` (the Partition Scheme setting is not used). Without the two files the sketch works on its own as before. See `EmuLauncher/README.md`.
- **VGA refresh** (`X1_VGA_HZ` at the top of `X1turbo.ino`): `55` = 55.5 Hz (default, same speed as the real machine). If your monitor does not show 55.5 Hz, choose `56`, `57` or `60`; the software then runs 1 %, 3 % or 8 % faster.

---

## 📂 SD Card Structure

```text
/ (SD Root)
└── /X1
    ├── /BIOS
    │   ├── IPLROM.X1T   : X1turbo IPL ROM 32KB (required)
    │   ├── FNT0808.X1   : ANK font 8x8 2KB (required)
    │   ├── FNT0816.X1   : ANK font 8x16 4KB (without it 8x16 text is not shown)
    │   └── FNT1616.X1   : Kanji font 306,176 bytes (without it kanji are not shown)
    └── <folders>/*.D88  : Disk images (sub-folders are allowed; shared with the X1 version). Raw images *.2D / *.FD (256 bytes x 16 sectors x 2 sides per cylinder, in order) can also be used.
```

The ROM files may also be placed directly in `/X1`.

---

## 🚀 Boot Menu

| Item | Description |
| :--- | :--- |
| Fd0 / Fd1 | Choose a disk image for each drive. For a file containing several disks, choose which one. If Fd0 is such a file and Fd1 is empty, the next disk goes to Fd1 automatically. The selector opens in the folder of the most recently chosen disk. |
| JoyToKey | ON / OFF: use controller 1 as keys (see Joystick) |
| Joy B1–B3 | The keys for controller buttons 1–3 while JoyToKey is ON. Press Enter, then the key (F6–F12 cancels). Saved in `/X1/JOYKEY.CFG`. |
| Next | The machine to start next. Shown only with EmuLauncher's two files (see Build Settings). Enter changes it; it takes effect at the next power-on or reset. |
| Run | Start |

The boot menu and the disk selectors can also be operated with the 1P controller: Up/Down = select (repeats while held), Left/Right = page up/down, button 1 = Enter, button 2 = cancel (ESC).

---

## 🕹️ Operations Guide

- **`[F12]`** : Pause / Resume (the pause message is shown at the bottom of the screen)
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

A PS/2 mouse on the mouse port of the board is connected to the emulated Z80 SIO (1F90h for the X1turbo, 1F98h for the mouse board CZ-8BM2). When software raises RTS of SIO channel B, three bytes (buttons, X movement, Y movement) are received. Not yet tested with real software.

---

## 🔧 Sound Tuning (for builders)

- `ym2151.h` `OPM_FB_CLIP_*`: limits the YM2151 operator-1 feedback to what can be reproduced at 16384 Hz (same as the PC-8801 emulator).
- `X1turbo.ino` `OPM_TREBLE_FIX`: treble correction of the FM output (0 = off).
- YM2151 CSM (each timer A overflow re-keys all channels, as in fmgen; used for speech) is supported.
- `x1.h` `X1_OPM_BOARD`: 0 = no FM sound board.

---

## Disclaimer

IN NO EVENT SHALL FABRIZIO DI VITTORIO, MARAT FAYZULLIN, OR THE DEVELOPER BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE. USE AT YOUR OWN RISK.

---

<a id="日本語"></a>
# 日本語

ESP32 と **FabGL** ライブラリを使った、高解像度（24kHz）モニタをつないだ シャープ X1turbo のエミュレータです（開発中）。VGA 出力、PS/2 キーボード・マウス、D88 / ベタのディスクイメージ、FM 音源ボード（YM2151）、シリアル接続の外部ジョイスティックに対応しています。

無印 X1 版（`X1` フォルダ）とは別のスケッチです。X1 版は無印用のソフトのための 15kHz（640×200）の機種、この版は 24kHz（640×400）の画面に固定した機種です。

## 📝 概要

- **画面**: 640×400・16 色（FabGL の VGA16Controller）。400 ライン（24kHz）の画面はそのまま、200 ラインの画面は 1 ラインを 2 回表示します。色 0〜7 がテキスト、8〜15 がグラフィックの色なので、パレットを書き換えても画面は描き直しません。
- **フレームレート**: 55.5Hz（24kHz の X1turbo と同じ）。フレームの始まりを VGA の画面に合わせるので、絵が裂けません。1 フレームのライン数と 1 ラインの CPU のサイクル数は実機と同じく CRTC の設定で決まります（帰線期間に PCG を書くタイミングがこれで決まるため）。CRTC を 15kHz に設定する無印用のソフトは約 1 割遅くなるので、X1 版を使ってください。1 フレームの処理が間に合わないときは、次のフレームの描画を省きます（エミュレーションの速さは保ちます）。
- **再現しているもの**: Z80（4MHz）、32KB の IPL ROM、サブ CPU（キーボード・ゲームキー・時計・割込みベクタ）、i8255 PPI、Z80 CTC、HD46505 CRTC とテキスト・PCG（PCG の読み書きは CRTC のビームの位置に合わせる）、640×200 / 320×200 / 400 ライン の 8 色グラフィック（パレット・優先度・同時書き込み）、AY-3-8910 PSG（2MHz）、MB8877 FDC と D88 イメージ（1 ファイルに複数枚入った D88 も可）・ベタの .2D / .FD イメージ、ジョイスティック端子。
- **turbo の機能**（Common Source Project と MAME の動作に合わせています）: 画面モード 1FD0h（高速 PCG、表示用 / CPU 用のグラフィック 2 ページ、400 ラインのグラフィック、ANK 8×16 フォント）、ブラッククリップ・テキストキー 1FE0h、漢字 VRAM（3800h）と漢字表示（JIS 第 1・第 2 水準、16 ラインの外字）、漢字 ROM の読み出し（0E80h〜0E82h）、Z80 DMA（1F80h。FDC と組み合わせて使う）、バンクメモリ 0B00h（32KB × 16）、Z80 CTC 1FA0h、Z80 SIO 1F90h、turbo のキーボード（CTRL・SHIFT・カナ・CAPS・GRAPH が変わったときも送る）、DIP スイッチ 1FF0h（高解像度モニタ・5 インチ 2D ドライブに固定）。
- **拡張ボード**: FM 音源ボード CZ-8BS1（YM2151 4MHz、0700h。専用の CTC は 0704h。CSM の音声にも対応）、マウスボード CZ-8BM2（Z80 SIO 1F98h、PS/2 マウス。ソフトでの確認はまだ）。
- **再現していないもの**: X1turboZ の機能（4096 色など）、VRAM アクセスのウエイト、漢字の下線、2HD のディスク、カセットテープ、RS-232C、プリンタ、YM2151 のステレオ。
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
- **PSRAM**（ON = `"Enabled"`、OFF = `"Disabled"`）: PSRAMをONにするとソフトでのエラッタ対策が入り、20%程度速度が低下します。PSRAMをOFFにするとソフトでPSRAMを有効にし、ソフトでのエラッタ対策は入りません。但し、ESP32のリビジョンが3以降でないと不具合が発生するため、それ以外のリビジョンでPSRAMをOFFにしても起動しなくしてあります。本プロジェクトではフレームスキップで実機に近い操作感を実現しており、PSRAMをOFFにすることでより60/55FPSに近くなります。
- **EmuLauncher での機種の切り替え**（使うときだけ）: `EmuLauncher` のフォルダの `emu_switch.h` と `partitions.csv` を、このスケッチのフォルダにコピーします。起動メニューに **Next**（次に起動する機種）が出て、フラッシュの分け方が `partitions.csv` のとおりになります（Partition Scheme の設定は使われません）。2 つのファイルが無ければ、今までどおり単体で動きます。詳しくは `EmuLauncher/README.md` を見てください。
- **VGA の縦の周波数**（`X1turbo.ino` の先頭の `X1_VGA_HZ`）: `55` = 55.5Hz（既定。実機と同じ速さ）。モニタに 55.5Hz が映らないときは `56`・`57`・`60` から選びます（そのぶんソフトが 1%・3%・8% 速くなります）。

---

## 📂 SD カードの構成

```text
/ (SD ルート)
└── /X1
    ├── /BIOS
    │   ├── IPLROM.X1T   : X1turbo の IPL ROM 32KB（必須）
    │   ├── FNT0808.X1   : ANK フォント 8×8 2KB（必須）
    │   ├── FNT0816.X1   : ANK フォント 8×16 4KB（ないと 8×16 の文字が出ない）
    │   └── FNT1616.X1   : 漢字フォント 306,176 バイト（ないと漢字が出ない）
    └── <フォルダ>/*.D88 : ディスクイメージ（フォルダ分けしてよい。X1 版と共通）。ベタのイメージ *.2D / *.FD（1 セクタ 256 バイト × 16 セクタ × 2 面を順に並べたもの）も使える
```

ROM のファイルは `/X1` の直下に置いてもかまいません。

---

## 🚀 起動メニュー

| 項目 | 内容 |
| :--- | :--- |
| Fd0 / Fd1 | 各ドライブのディスクイメージを選ぶ。複数枚入ったファイルは何枚目かを選ぶ。Fd0 に複数枚入ったファイルを選び Fd1 が空なら、次の 1 枚を自動で Fd1 に入れる。選択画面は、最後に選んだディスクのフォルダから開く。 |
| JoyToKey | ON / OFF: コントローラ 1 をキーとして使う（「ジョイスティック」を参照） |
| Joy B1〜B3 | JoyToKey が ON のときの、コントローラのボタン 1〜3 のキー。Enter を押してからキーを押す（F6〜F12 で取り消し）。`/X1/JOYKEY.CFG` に保存。 |
| Next | 次に起動する機種。EmuLauncher の 2 つのファイルを置いたときだけ出ます（ビルド設定を参照）。Enter で変わり、電源を入れ直すかリセットすると切り替わります。 |
| Run | 起動 |

起動メニューとディスクの選択画面は、1P のコントローラでも操作できます：↑↓ = 選択（押し続けると連続）、←→ = 1画面送り、ボタン 1 = 決定（Enter）、ボタン 2 = 取り消し（ESC）。

---

## 🕹️ 操作方法

- **`[F12]`** : 一時停止 / 再開（一時停止の表示は画面のいちばん下に出ます）
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

ボードのマウス端子につないだ PS/2 マウスを、Z80 SIO（X1turbo 本体の 1F90h と、マウスボード CZ-8BM2 の 1F98h）につなぎます。ソフトが SIO のチャンネル B の RTS を立てると、3 バイト（ボタン・X の移動量・Y の移動量）が届きます。実際のソフトでの確認はまだです。

---

## 🔧 音の調整（ビルドする人向け）

- `ym2151.h` の `OPM_FB_CLIP_*`: YM2151 のオペレータ 1 のフィードバックを、16384Hz で再現できる強さまでに制限する（PC-8801 版と同じ）。
- `X1turbo.ino` の `OPM_TREBLE_FIX`: FM の高音の補正（0 で補正なし）。
- YM2151 の CSM（タイマー A があふれるたびに全チャンネルをキーオンし直す。fmgen と同じ。音声合成に使われる）に対応しています。
- `x1.h` の `X1_OPM_BOARD`: 0 で FM 音源ボードなし。

---

## 免責事項

本ソフトウェアの使用によって生じたいかなる損害についても、FABRIZIO DI VITTORIO 氏、MARAT FAYZULLIN 氏、および開発者は一切の責任を負いません。自己責任でご利用ください。
