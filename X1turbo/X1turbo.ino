/* ============================================================================
 *  Sharp X1turbo Emulator for ESP32 (Work in Progress)
 *  --- 無印 X1 版(Desktop\emu\X1)から分けた turbo 版。画面は 640x400 16色(24kHz)固定 ---
 * ============================================================================
 *
 * 【クレジットと謝辞】
 *  - グラフィック/オーディオエンジン: Fabrizio Di Vittorio氏による FabGL
 *  - Z80 CPUコア: Marat Fayzullin氏によるコードをベースに改変
 *
 * 【必要なファイル（SDカード）】
 *  /X1/BIOS/IPLROM.X1T : turbo の IPL ROM (32KB)
 *  /X1/BIOS/FNT0808.X1 : ANKフォント (2KB, 8x8x256文字)
 *  /X1/BIOS/FNT0816.X1 : ANKフォント (4KB, 8x16x256文字)
 *  /X1/BIOS/FNT1616.X1 : 漢字フォント (306,176 バイト)
 *  /X1/*.D88           : ディスクイメージ(/X1 の中のフォルダに置いてもよい。起動メニューで入れる)
 *
 * 【開発状況】方針B(プロトコル近似)で実装。
 *  カセットテープは対応しない。
 * ============================================================================
 */

#pragma GCC optimize("O3")

#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <SPI.h>
#include <SD.h>
#include <fabgl.h>
#include <vector>
#include <algorithm>
#include "soc/rtc_wdt.h"
#include "esp_task_wdt.h"
#ifndef BOARD_HAS_PSRAM
#include "esp32/spiram.h"
#include "esp_heap_caps.h"
#define ps_malloc(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#endif
#if defined(BOARD_HAS_PSRAM) && !defined(EMU_PSRAM_NO_ERRATA_FIX)
#define psramFixMissing() false
#else
#define psramFixMissing() (ESP.getChipRevision() < 3)
#endif
#include "Z80.h"
#include "x1.h"

//#define USE_SERIAL_DEBUG

#define X1_VGA_HZ 55
#if X1_VGA_HZ == 55
#define X1_VGA_VTOTAL 567
#elif X1_VGA_HZ == 56
#define X1_VGA_VTOTAL 562
#elif X1_VGA_HZ == 57
#define X1_VGA_VTOTAL 552
#elif X1_VGA_HZ == 60
#define X1_VGA_VTOTAL 524
#else
#error "X1_VGA_HZ は 55 / 56 / 57 / 60 のどれか"
#endif
#define X1_FRAME_US (800 * X1_VGA_VTOTAL * 1000 / 25175)

#define SD_SCLK 14
#define SD_MISO 2
//#define SD_MISO 35  // ORANGE ESPer
#define SD_MOSI 12
#define SD_CS 13

struct D88Image { uint32_t ofs, size; char name[18]; };
static const int D88_MAX_IMAGES = 16;

extern "C" {
  extern Z80 cpu;
  byte RdZ80(zword Addr);
  void WrZ80(zword Addr, byte Value);
  byte InZ80(zword Port);
  void OutZ80(zword Port, byte Value);
  void PatchZ80(Z80* R);
}

Z80 cpu;

#ifdef X1_RAM_STATS
extern uint32_t x1_ram_hits[X1_RAM_PAGES];
#define X1_HIT(a) (x1_ram_hits[(a) >> X1_RAM_PAGE_SHIFT]++)
#else
#define X1_HIT(a) ((void)0)
#endif
byte IRAM_ATTR RdZ80(zword Addr) { X1_HIT(Addr); return x1_rd_map[Addr >> 12][Addr & 0x0FFF]; }
void IRAM_ATTR WrZ80(zword Addr, byte Value) { X1_HIT(Addr); x1_wr_map[Addr >> 12][Addr & 0x0FFF] = Value; }
byte IRAM_ATTR InZ80(zword Port) { return x1ReadIO(Port); }
void IRAM_ATTR OutZ80(zword Port, byte Value) {
  x1WriteIO(Port, Value);
  cpu.ICount -= x1DmaTakeCycles();
}
void PatchZ80(Z80* R) {}

int x1LineCycle() {
  int c = x1_cyc_line - cpu.ICount;
  return c < 0 ? 0 : c >= x1_cyc_line ? x1_cyc_line - 1 : c;
}

class X1VGA16 : public fabgl::VGA16Controller {
public:
  uint8_t **rowTable() { return (uint8_t **)m_viewPort; }
  static int scanLine() { return s_scanLine; }
};

X1VGA16 DisplayController;
fabgl::Canvas* canvas;
fabgl::SoundGenerator soundGenerator;
fabgl::PS2Controller PS2Controller;

volatile bool frameReady = false;
static volatile bool resetRequest = false;
static volatile bool isPaused = false;

void x1SetPaletteEntry(int index, uint8_t r, uint8_t g, uint8_t b) {
  DisplayController.setPaletteItem(index, fabgl::RGB888(r ? 255 : 0, g ? 255 : 0, b ? 255 : 0));
}
static void menuPalette() {
  for (int i = 0; i < 16; i++) DisplayController.setPaletteItem(i, fabgl::RGB888((fabgl::Color)i));
}

static volatile uint8_t joyMsx[2] = { 0x3F, 0x3F };

static void updateJoystick(bool hold) {
  uint8_t j[2] = { 0x3F, 0x3F };
  if (hold) { j[0] = joyMsx[0]; j[1] = joyMsx[1]; }
  while (Serial1.available() > 0) {
    uint8_t b = Serial1.read();
    j[(b & 0x80) ? 1 : 0] = b & 0x3F;
  }
  joyMsx[0] = j[0];
  joyMsx[1] = j[1];
}

static int joyMenuPoll() {
  static uint8_t state = 0x7F, last = 0x7F;
  static uint32_t lastRx = 0, repeatAt = 0;
  uint32_t now = millis();
  while (Serial1.available() > 0) {
    uint8_t b = Serial1.read();
    if (!(b & 0x80)) { state = b & 0x7F; lastRx = now; }
  }
  if (now - lastRx > 100) state = 0x7F;
  uint8_t pressed = (uint8_t)(~state & 0x7F), prev = (uint8_t)(~last & 0x7F);
  uint8_t edge = pressed & ~prev;
  last = state;
  if (edge & 0x10) return 'A';
  if (edge & 0x20) return 'B';
  static const char dirs[4] = { 'U', 'D', 'L', 'R' };
  for (int i = 0; i < 4; i++) {
    if (!(pressed & (1 << i))) continue;
    if (edge & (1 << i)) { repeatAt = now + 400; return dirs[i]; }
    if ((int32_t)(now - repeatAt) >= 0) { repeatAt = now + 120; return dirs[i]; }
  }
  return 0;
}
static int joyMenuScancode(bool *e0) {
  *e0 = true;
  switch (joyMenuPoll()) {
    case 'U': return 0x75;
    case 'D': return 0x72;
    case 'L': return 0x7D;
    case 'R': return 0x7A;
    case 'A': *e0 = false; return 0x5A;
    case 'B': *e0 = false; return 0x76;
    default: return 0;
  }
}

static volatile uint8_t kbdJoy = 0x3F;

static volatile uint8_t kbdMatrix[X1_KEY_ROWS] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static bool joyToKeyOn = false;
static uint8_t joyButtonKey[3] = { 0x20, 0x0D, 0x1B };
static const char *JOYKEY_FILE = "/X1/JOYKEY.CFG";

static inline uint8_t x1JoyPort(int n) {
  uint8_t j = (n == 0 && joyToKeyOn) ? 0x3F : joyMsx[n];
  uint8_t m = j & (n == 0 ? kbdJoy : 0x3F);
  return 0x90 | (m & 0x0F) | ((m & 0x30) << 1);
}

static void joyToKeys() {
  uint8_t joy[X1_KEY_ROWS];
  memset(joy, 0xFF, sizeof(joy));
  if (joyToKeyOn) {
    uint8_t m = joyMsx[0];
    bool up = !(m & 1), dn = !(m & 2), lt = !(m & 4), rt = !(m & 8);
    int ten = -1;
    if (up) ten = lt ? 7 : rt ? 9 : 8;
    else if (dn) ten = lt ? 1 : rt ? 3 : 2;
    else if (lt) ten = 4;
    else if (rt) ten = 6;
    if (ten >= 0) { int k = X1K_TEN0 + ten; joy[k >> 3] &= (uint8_t)~(1 << (k & 7)); }
    for (int b = 0; b < 3; b++)
      if (!(m & (0x10 << b))) joy[joyButtonKey[b] >> 3] &= (uint8_t)~(1 << (joyButtonKey[b] & 7));
  }
  for (int r = 0; r < X1_KEY_ROWS; r++) x1_key_matrix[r] = kbdMatrix[r] & joy[r];
}

static const char *x1KeyName(uint8_t k) {
  static char buf[8];
  switch (k) {
    case 0x03: return "BREAK"; case 0x08: return "DEL"; case 0x09: return "TAB"; case 0x0B: return "HOME";
    case 0x0D: return "RETURN"; case 0x12: return "INS"; case 0x1B: return "ESC"; case 0x1C: return "RIGHT";
    case 0x1D: return "LEFT"; case 0x1E: return "UP"; case 0x1F: return "DOWN"; case 0x20: return "SPACE";
    case X1K_CTRL: return "CTRL"; case X1K_SHIFT: return "SHIFT"; case X1K_KANA: return "KANA";
    case X1K_CAPS: return "CAPS"; case X1K_GRAPH: return "GRAPH"; case X1K_TENCOMMA: return "NUM,";
    default: break;
  }
  if (k >= X1K_TEN0 && k <= X1K_TEN0 + 15) {
    static const char *ten = "0123456789-+*/.=";
    snprintf(buf, sizeof(buf), "NUM%c", ten[k - X1K_TEN0]);
    return buf;
  }
  if (k >= X1K_F1 && k <= X1K_F1 + 4) { snprintf(buf, sizeof(buf), "F%d", k - X1K_F1 + 1); return buf; }
  if (k > 0x20 && k < 0x60) { snprintf(buf, sizeof(buf), "%c", k); return buf; }
  return "?";
}

static void loadJoyKeys() {
  File f = SD.open(JOYKEY_FILE, FILE_READ);
  if (!f) return;
  uint8_t b[4];
  if (f.read(b, 4) == 4) {
    joyToKeyOn = b[0] != 0;
    for (int i = 0; i < 3; i++) if (b[i + 1] < 0x80) joyButtonKey[i] = b[i + 1];
  }
  f.close();
}
static void saveJoyKeys() {
  File f = SD.open(JOYKEY_FILE, FILE_WRITE);
  if (!f) return;
  uint8_t b[4] = { (uint8_t)(joyToKeyOn ? 1 : 0), joyButtonKey[0], joyButtonKey[1], joyButtonKey[2] };
  f.write(b, 4);
  f.close();
}

static const int AY_BUFFER_SIZE = 512;

#if X1_OPM_BOARD
#include "ym2151.h"
static const int OPM_QUEUE = 1024;
static volatile uint32_t *opmQueue = nullptr;
static volatile int opmQHead = 0, opmQTail = 0;
static volatile uint32_t opmEmuClock = 0;
static volatile bool opmResetRequest = false;
#define OPM_TREBLE_FIX 320

static void opmQueuePush(uint32_t t, uint8_t reg, uint8_t val) {
  int next = (opmQHead + 1) % OPM_QUEUE;
  if (next == opmQTail || !opmQueue) return;
  opmQueue[opmQHead] = ((t >> 5) << 16) | ((uint32_t)reg << 8) | val;
  opmQHead = next;
}
void x1OpmWrite(uint8_t reg, uint8_t val) { opmQueuePush(opmEmuClock + (uint32_t)x1LineCycle(), reg, val); }
void x1OpmCsm(uint32_t at) { opmQueuePush(opmEmuClock + at, 0x02, 0); }

#endif

static volatile bool psg2scc = false;
static const int8_t SCC_PRESET[3][32] = {
  { 0x00, 0x13, 0x26, 0x39, 0x4A, 0x60, 0x76, 0x5D, 0x00, (int8_t)0xA3, (int8_t)0x8A, (int8_t)0xA0, (int8_t)0xB6, (int8_t)0xC7, (int8_t)0xDA, (int8_t)0xED,
    0x00, 0x13, 0x26, 0x39, 0x4A, 0x60, 0x76, 0x5D, 0x00, (int8_t)0xA3, (int8_t)0x8A, (int8_t)0xA0, (int8_t)0xB6, (int8_t)0xC7, (int8_t)0xDA, (int8_t)0xED },
  { 0x29, 0x6D, 0x7C, 0x79, 0x7B, 0x79, 0x7C, 0x6D, 0x29, (int8_t)0xE5, (int8_t)0xD5, (int8_t)0xD8, (int8_t)0xD7, (int8_t)0xD8, (int8_t)0xD7, (int8_t)0xD7,
    (int8_t)0xD7, (int8_t)0xD7, (int8_t)0xD7, (int8_t)0xD7, (int8_t)0xD7, (int8_t)0xD7, (int8_t)0xD7, (int8_t)0xD7, (int8_t)0xD7, (int8_t)0xD7, (int8_t)0xD7, (int8_t)0xD8, (int8_t)0xD7, (int8_t)0xD8, (int8_t)0xD5, (int8_t)0xE5 },
  { 0x00, 0x1A, 0x34, 0x50, 0x69, 0x76, 0x6A, 0x40, 0x00, (int8_t)0xC0, (int8_t)0x96, (int8_t)0x8A, (int8_t)0x97, (int8_t)0xB0, (int8_t)0xCC, (int8_t)0xE6,
    0x00, 0x1A, 0x34, 0x50, 0x69, 0x76, 0x6A, 0x40, 0x00, (int8_t)0xC0, (int8_t)0x96, (int8_t)0x8A, (int8_t)0x97, (int8_t)0xB0, (int8_t)0xCC, (int8_t)0xE6 } };
static const uint8_t SCC_VOL_TABLE[16] = { 0, 2, 4, 7, 10, 15, 21, 26, 33, 39, 47, 54, 62, 68, 73, 75 };

class AY8910Generator : public fabgl::WaveformGenerator {
private:
  uint32_t sccPhase[3] = { 0, 0, 0 };
  volatile uint32_t sccInc[3] = { 0, 0, 0 };
#if X1_OPM_BOARD
  Ym2151  *opm = nullptr;
  int32_t  opmBuf[64];
  int32_t  fmLp = 0;
  uint32_t sndClock = 0, sndFrac = 0, clkPerSampleQ16 = 0, clkFrame = 0;
  void updateClkPerSample(uint32_t frameClk) {
    int rate = sampleRate() > 0 ? sampleRate() : 16384;
    const uint64_t cpuHz = (uint64_t)frameClk * 1000000u / X1_FRAME_US;
    clkPerSampleQ16 = (uint32_t)((cpuHz << 16) / rate);
    clkFrame = frameClk;
  }
  void applyQueued() {
    uint32_t e = opmQueue[opmQTail];
    opmQTail = (opmQTail + 1) % OPM_QUEUE;
    if (((e >> 8) & 0xFF) == 0x02) ym2151CsmKeyOn(opm);
    else ym2151WriteReg(opm, (e >> 8) & 0xFF, e & 0xFF);
  }
  void advanceClock(int n, uint32_t emu) {
    uint32_t f = sndFrac + (uint32_t)n * (clkPerSampleQ16 & 0xFFFF);
    sndClock += (uint32_t)n * (clkPerSampleQ16 >> 16) + (f >> 16);
    sndFrac = f & 0xFFFF;
    if ((int32_t)(sndClock - emu) > 0) { sndClock = emu; sndFrac = 0; }
  }
#endif
  const uint32_t psgClock = 2000000 / 16;
  uint32_t tickAccum = 0, tickStep = 0;
  uint8_t  addrLatch = 0;
  volatile uint32_t tonePeriod[3] = { 1, 1, 1 };
  volatile uint32_t noisePeriod = 1, envPeriod = 1;
  volatile uint8_t regs[16] = { 0 };
  volatile bool holding = true;
  volatile bool cont = false, alt = false, hold = false, att = false;
  volatile uint32_t envCounter = 0, envStep = 0, envVol = 0;
  uint32_t toneCounter[3] = { 0, 0, 0 };
  uint32_t toneOut = 0, noiseCounter = 0, lfsr = 1, noiseOut = 0;
  int16_t volTable[16] = { 0, 1, 1, 2, 3, 4, 6, 8, 11, 16, 22, 32, 45, 64, 90, 127 };
  int32_t dcPrev = 0, dcOut = 0;
  int16_t sampleBuffer[AY_BUFFER_SIZE];
  volatile int head = 0, tail = 0;
  int lastSample = 0;

public:
  void selectAddr(uint8_t a) { addrLatch = a & 0x0F; }
  void writeData(uint8_t val) {
    uint8_t reg = addrLatch;
    regs[reg] = val;
    switch (reg) {
      case 0: case 1: case 2: case 3: case 4: case 5: {
        int ch = reg >> 1;
        uint32_t p = regs[ch * 2] | ((regs[ch * 2 + 1] & 0x0F) << 8);
        tonePeriod[ch] = p ? p : 1;
        sccInc[ch] = p > 10 ? (uint32_t)(((uint64_t)psgClock << 18) / p) : 0;
        break;
      }
      case 6: noisePeriod = (regs[6] & 0x1F) ? (regs[6] & 0x1F) : 1; break;
      case 11: case 12: { uint32_t p = regs[11] | (regs[12] << 8); envPeriod = p ? p : 1; break; }
      case 13: {
        uint8_t shape = val & 0x0F;
        envStep = 0; envCounter = 0;
        cont = shape & 0x08; att = shape & 0x04; alt = shape & 0x02; hold = shape & 0x01;
        envVol = att ? 0 : 15;
        holding = false;
        break;
      }
    }
  }
  uint8_t readData() {
    if (addrLatch == 14) return x1JoyPort(0);
    if (addrLatch == 15) return x1JoyPort(1);
    return regs[addrLatch];
  }

  void fillBuffer() {
    if (tickStep == 0) {
      int rate = sampleRate();
      if (rate > 0) setSampleRate(rate); else return;
    }
    if (isPaused) {
      int space = (tail - head - 1 + AY_BUFFER_SIZE) % AY_BUFFER_SIZE;
      while (space-- > 0) { sampleBuffer[head] = 0; head = (head + 1) % AY_BUFFER_SIZE; }
      return;
    }
#if X1_OPM_BOARD
    if (!opm) {
      if (!opmQueue) return;
      opm = ym2151Create(X1_OPM_CLOCK, sampleRate() > 0 ? sampleRate() : 16384);
      if (!opm) return;
    }
    const uint32_t frameClk = (uint32_t)x1_cyc_line * x1_lines_frame;
    if (frameClk != clkFrame) updateClkPerSample(frameClk);
    uint32_t emu = opmEmuClock;
    if (opmResetRequest) {
      opmResetRequest = false;
      opmQTail = opmQHead;
      ym2151Reset(opm);
      sndClock = emu; sndFrac = 0;
    }
    if ((int32_t)(emu - sndClock) > (int32_t)(frameClk + frameClk / 4)) {
      while (opmQTail != opmQHead) applyQueued();
      sndClock = emu - frameClk; sndFrac = 0;
    }
#endif
    int space = (tail - head - 1 + AY_BUFFER_SIZE) % AY_BUFFER_SIZE;
    while (space > 0) {
      int n = space > 64 ? 64 : space;
#if X1_OPM_BOARD
      while (opmQTail != opmQHead) {
        uint32_t t16 = opmQueue[opmQTail] >> 16;
        int32_t d = (int16_t)(t16 - (sndClock >> 5));
        if (d > 0) {
          if ((int16_t)(t16 - (emu >> 5)) <= 0) {
            uint32_t per = (clkPerSampleQ16 >> 16) + 1;
            int m = (int)((((uint32_t)d << 5) + per - 1) / per);
            if (m < n) n = m;
          }
          break;
        }
        applyQueued();
      }
      advanceClock(n, emu);
      ym2151Generate(opm, opmBuf, n);
#endif
      for (int i = 0; i < n; i++) {
        int32_t x = calculateNextSample();
#if X1_OPM_BOARD
#if OPM_TREBLE_FIX
        fmLp += ((opmBuf[i] - fmLp) * OPM_TREBLE_FIX) >> 8;
#else
        fmLp = opmBuf[i];
#endif
        x += fmLp >> 7;
#endif
        x <<= 8;
        dcOut = x - dcPrev + dcOut - (dcOut >> 8);
        dcPrev = x;
        int32_t s = dcOut >> 8;
        if (s > 96) s = 96 + (s - 96) / 3; else if (s < -96) s = -96 + (s + 96) / 3;
        if (s > 127) s = 127; else if (s < -128) s = -128;
        sampleBuffer[head] = (int16_t)s;
        head = (head + 1) % AY_BUFFER_SIZE;
      }
      space -= n;
    }
  }
  void setSampleRate(int rate) {
    this->fabgl::WaveformGenerator::setSampleRate(rate);
    tickStep = (uint32_t)(((uint64_t)psgClock << 16) / (rate > 0 ? rate : 16384));
#if X1_OPM_BOARD
    updateClkPerSample((uint32_t)x1_cyc_line * x1_lines_frame);
#endif
  }

private:
  inline int calculateNextSample() {
    uint32_t r7 = regs[7], r8 = regs[8], r9 = regs[9], r10 = regs[10];
    uint32_t tp0 = tonePeriod[0], tp1 = tonePeriod[1], tp2 = tonePeriod[2], np = noisePeriod;
    tickAccum += tickStep;
    uint32_t ticks = tickAccum >> 16;
    tickAccum &= 0xFFFF;
    while (ticks--) {
      toneCounter[0] += 2; while (toneCounter[0] >= tp0) { toneCounter[0] -= tp0; toneOut ^= 1; }
      toneCounter[1] += 2; while (toneCounter[1] >= tp1) { toneCounter[1] -= tp1; toneOut ^= 2; }
      toneCounter[2] += 2; while (toneCounter[2] >= tp2) { toneCounter[2] -= tp2; toneOut ^= 4; }
      if (++noiseCounter >= np) {
        noiseCounter = 0;
        lfsr = (lfsr >> 1) | (((lfsr & 1) ^ ((lfsr >> 3) & 1)) << 16);
        noiseOut = lfsr & 1;
      }
      if (!holding && ++envCounter >= envPeriod) { envCounter = 0; stepEnvelope(); }
    }
    uint32_t mix = (toneOut | r7) & ((noiseOut ? 0x07 : 0) | (r7 >> 3));
    int volA = volTable[(r8 & 0x10) ? envVol : (r8 & 0x0F)];
    int volB = volTable[(r9 & 0x10) ? envVol : (r9 & 0x0F)];
    int volC = volTable[(r10 & 0x10) ? envVol : (r10 & 0x0F)];
    if ((r7 & 0x09) == 0x01) volA >>= 1;
    if ((r7 & 0x12) == 0x02) volB >>= 1;
    if ((r7 & 0x24) == 0x04) volC >>= 1;
    int s = 0;
    if (mix & 1) s += volA;
    if (mix & 2) s += volB;
    if (mix & 4) s += volC;
    int scc = 0;
    if (psg2scc) {
      uint32_t en = ~r7 & 7;
      for (int c = 0; c < 3; c++) {
        uint32_t inc = sccInc[c];
        if (!((en >> c) & 1) || !inc) continue;
        scc += (SCC_PRESET[c][(sccPhase[c] >> 27) & 31] * SCC_VOL_TABLE[regs[8 + c] & 0x0F]) >> 8;
        sccPhase[c] += inc;
      }
    }
    return ((s * 171) >> 8) + ((scc * 4) / 3);
  }
  int getSample() override {
    if (head == tail) return lastSample;
    lastSample = sampleBuffer[tail];
    tail = (tail + 1) % AY_BUFFER_SIZE;
    return lastSample;
  }
  inline void stepEnvelope() {
    if (envStep < 15) {
      envStep++;
      envVol = att ? envStep : (15 - envStep);
    } else if (!cont) {
      holding = true; envVol = 0;
    } else if (hold) {
      holding = true;
      if (alt) att = !att;
      envVol = att ? 15 : 0;
    } else {
      envStep = 0;
      if (alt) att = !att;
      envVol = att ? 0 : 15;
    }
  }
  void setFrequency(int value) override {}
};

AY8910Generator psgGen;

void x1PsgSelectAddr(uint8_t addr) { psgGen.selectAddr(addr); }
void x1PsgWrite(uint8_t val) { psgGen.writeData(val); }
uint8_t x1PsgRead() { return psgGen.readData(); }

static uint8_t *x1_ram_page[X1_RAM_PAGES];
static uint8_t *x1_ipl_rom;
static uint8_t *x1_cgrom;
static uint8_t *x1_tvram;
static uint8_t *x1_avram;
static uint8_t *x1_kvram;
static uint8_t *x1_kanji;
static uint8_t *x1_extram;
static uint8_t *x1_pcg[3];
static uint8_t *x1_gvram;

#define NUM_DRIVES 2
static X1Drive x1_drives[NUM_DRIVES];
static String  drive_path[NUM_DRIVES];
static uint32_t driveOfs[NUM_DRIVES];
static bool driveRaw[NUM_DRIVES];

static bool isRawImage(const String &path) {
  String u = path; u.toUpperCase();
  return u.endsWith(".2D") || u.endsWith(".FD");
}

char msgBuf[64];

static void flashStatus(const char *msg, int waitMs = 0) {
  Serial.println(msg);
}

struct X1KeyMap {
  uint8_t ps2;
  bool    ext;
  uint8_t key;
};

static const X1KeyMap keyMapTable[] = {
  { 0x76, false, 0x1B }, { 0x0D, false, 0x09 }, { 0x5A, false, 0x0D }, { 0x29, false, 0x20 },
  { 0x66, false, 0x08 },
  { 0x16, false, '1' }, { 0x1E, false, '2' }, { 0x26, false, '3' }, { 0x25, false, '4' }, { 0x2E, false, '5' },
  { 0x36, false, '6' }, { 0x3D, false, '7' }, { 0x3E, false, '8' }, { 0x46, false, '9' }, { 0x45, false, '0' },
  { 0x4E, false, '-' }, { 0x55, false, '^' }, { 0x6A, false, 0x5C },
  { 0x15, false, 'Q' }, { 0x1D, false, 'W' }, { 0x24, false, 'E' }, { 0x2D, false, 'R' }, { 0x2C, false, 'T' },
  { 0x35, false, 'Y' }, { 0x3C, false, 'U' }, { 0x43, false, 'I' }, { 0x44, false, 'O' }, { 0x4D, false, 'P' },
  { 0x54, false, '@' }, { 0x5B, false, '[' },
  { 0x1C, false, 'A' }, { 0x1B, false, 'S' }, { 0x23, false, 'D' }, { 0x2B, false, 'F' }, { 0x34, false, 'G' },
  { 0x33, false, 'H' }, { 0x3B, false, 'J' }, { 0x42, false, 'K' }, { 0x4B, false, 'L' },
  { 0x4C, false, ';' }, { 0x52, false, ':' }, { 0x5D, false, ']' },
  { 0x1A, false, 'Z' }, { 0x22, false, 'X' }, { 0x21, false, 'C' }, { 0x2A, false, 'V' }, { 0x32, false, 'B' },
  { 0x31, false, 'N' }, { 0x3A, false, 'M' }, { 0x41, false, ',' }, { 0x49, false, '.' }, { 0x4A, false, '/' },
  { 0x51, false, '_' },
  { 0x74, true, 0x1C }, { 0x6B, true, 0x1D }, { 0x75, true, 0x1E }, { 0x72, true, 0x1F },
  { 0x6C, true, 0x0B }, { 0x70, true, 0x12 }, { 0x71, true, 0x08 }, { 0x69, true, 0x03 },
  { 0x70, false, X1K_TEN0 + 0 }, { 0x69, false, X1K_TEN0 + 1 }, { 0x72, false, X1K_TEN0 + 2 }, { 0x7A, false, X1K_TEN0 + 3 },
  { 0x6B, false, X1K_TEN0 + 4 }, { 0x73, false, X1K_TEN0 + 5 }, { 0x74, false, X1K_TEN0 + 6 }, { 0x6C, false, X1K_TEN0 + 7 },
  { 0x75, false, X1K_TEN0 + 8 }, { 0x7D, false, X1K_TEN0 + 9 }, { 0x7B, false, X1K_TEN0 + 10 }, { 0x79, false, X1K_TEN0 + 11 },
  { 0x7C, false, X1K_TEN0 + 12 }, { 0x4A, true, X1K_TEN0 + 13 }, { 0x71, false, X1K_TEN0 + 14 },
  { 0x5A, true, 0x0D },
  { 0x05, false, X1K_F1 + 0 }, { 0x06, false, X1K_F1 + 1 }, { 0x04, false, X1K_F1 + 2 }, { 0x0C, false, X1K_F1 + 3 },
  { 0x03, false, X1K_F1 + 4 },
  { 0x12, false, X1K_SHIFT }, { 0x59, false, X1K_SHIFT }, { 0x14, false, X1K_CTRL }, { 0x14, true, X1K_CTRL },
  { 0x11, false, X1K_GRAPH }, { 0x11, true, X1K_GRAPH }, { 0x64, false, X1K_GRAPH }, { 0x67, false, X1K_GRAPH },
};
static const int keyMapCount = sizeof(keyMapTable) / sizeof(keyMapTable[0]);
static bool capsLock = false;

static void writeBackD88(int driveNo);
static void swapDisk(int d);
static void showStatus(const char *msg);
static const char *PAUSE_MSG = "PAUSED  F12:RESUME  F10/F11:FD0/FD1  SHIFT+F10/F11:WRITE BACK  SHIFT+F12:RESET";

static uint8_t kbdKept[32];
static int kbdKeptHead = 0, kbdKeptCount = 0;
static bool kbdLedByte(uint8_t b) {
  fabgl::PS2Controller::sendData(b, 0);
  for (uint32_t t0 = millis(); millis() - t0 < 500; ) {
    int r = fabgl::PS2Controller::getData(0, 10);
    if (r == 0xFA) return true;
    if (r >= 0 && r != 0xFE && kbdKeptCount < (int)sizeof(kbdKept)) kbdKept[(kbdKeptHead + kbdKeptCount++) % sizeof(kbdKept)] = (uint8_t)r;
  }
  return false;
}
static void kbdSetLeds(bool capsLock, bool scrollLock) {
  fabgl::PS2Controller::lock(0, -1);
  {
    fabgl::PS2PortAutoDisableRX mouseOff(1);
    if (kbdLedByte(0xED)) kbdLedByte((scrollLock ? 1 : 0) | (capsLock ? 4 : 0));
  }
  fabgl::PS2Controller::unlock(0);
}
static int kbdAvailable(fabgl::Keyboard *kb) { return kbdKeptCount + kb->scancodeAvailable(); }
static int kbdNextScancode(fabgl::Keyboard *kb) {
  if (kbdKeptCount > 0) {
    uint8_t b = kbdKept[kbdKeptHead];
    kbdKeptHead = (kbdKeptHead + 1) % (int)sizeof(kbdKept);
    kbdKeptCount--;
    return b;
  }
  return kb->getNextScancode(0);
}

void updateKeyboard() {
  static bool isBreak = false;
  static bool isE0 = false;
  static bool isShift = false;
  static bool f8_held = false;
  static bool f12_held = false;
  static bool f10_held = false, f11_held = false;

  static uint8_t deferRel[X1_KEY_ROWS];
  for (int r = 0; r < X1_KEY_ROWS; r++) if (deferRel[r]) { kbdMatrix[r] |= deferRel[r]; deferRel[r] = 0; }
  uint8_t pressedNow[X1_KEY_ROWS] = { 0 };

  auto kb = PS2Controller.keyboard();
  if (!kb) return;
  int count = kbdAvailable(kb);
  if (count <= 0) return;

  while (count--) {
    int sc = kbdNextScancode(kb);
    if (sc == -1) break;
    if (sc == 0xF0) { isBreak = true; continue; }
    if (sc == 0xE0) { isE0 = true; continue; }
    if (sc == 0x12 || sc == 0x59) { isShift = !isBreak; }

    if (sc == 0x07 && !isE0) {
      if (!isBreak && !f12_held) {
        f12_held = true;
        if (isShift) esp_restart();
        isPaused = !isPaused;
        if (isPaused) { vTaskDelay(pdMS_TO_TICKS(40)); showStatus(PAUSE_MSG); }
        else x1VideoResume();
      } else if (isBreak) f12_held = false;
      isBreak = false; isE0 = false;
      continue;
    }

    if ((sc == 0x09 || sc == 0x78) && !isE0) {
      bool &held = (sc == 0x09) ? f10_held : f11_held;
      int d = (sc == 0x09) ? 0 : 1;
      if (!isBreak && !held) {
        held = true;
        if (isPaused) {
          if (isShift) writeBackD88(d);
          else swapDisk(d);
        }
      } else if (isBreak) held = false;
      isBreak = false; isE0 = false;
      continue;
    }
    if (sc == 0x0A && !isE0) {
      if (!isBreak && !f8_held) {
        f8_held = true;
        psg2scc = !psg2scc;
        flashStatus(psg2scc ? "SCC ON" : "SCC OFF");
      } else if (isBreak) f8_held = false;
      isBreak = false; isE0 = false;
      continue;
    }

    if (sc == 0x58 && !isE0) {
      if (!isBreak) {
        capsLock = !capsLock;
        if (capsLock) kbdMatrix[X1K_CAPS >> 3] &= (uint8_t)~(1 << (X1K_CAPS & 7));
        else          kbdMatrix[X1K_CAPS >> 3] |= (uint8_t)(1 << (X1K_CAPS & 7));
        kbdSetLeds(capsLock, false);
      }
      isBreak = false; isE0 = false;
      continue;
    }

    if (isE0 && (sc == 0x75 || sc == 0x72 || sc == 0x6B || sc == 0x74)) {
      uint8_t jb = sc == 0x75 ? 0x01 : sc == 0x72 ? 0x02 : sc == 0x6B ? 0x04 : 0x08;
      if (isBreak) kbdJoy |= jb; else kbdJoy &= (uint8_t)~jb;
    }

    for (int i = 0; i < keyMapCount; i++) {
      if (keyMapTable[i].ps2 == sc && keyMapTable[i].ext == isE0) {
        uint8_t k = keyMapTable[i].key, mask = (uint8_t)(1 << (k & 7));
        if (isBreak) {
          if (pressedNow[k >> 3] & mask) deferRel[k >> 3] |= mask;
          else kbdMatrix[k >> 3] |= mask;
        } else {
          kbdMatrix[k >> 3] &= (uint8_t)~mask;
          pressedNow[k >> 3] |= mask;
          deferRel[k >> 3] &= (uint8_t)~mask;
        }

        break;
      }
    }
    isBreak = false; isE0 = false;
  }
}

static bool loadFileToBuf(const char *path, uint8_t *buf, uint32_t maxSize) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  uint32_t sz = f.size();
  if (sz > maxSize) sz = maxSize;
  uint32_t done = 0;
  while (done < sz) {
    size_t chunk = min((size_t)4096, (size_t)(sz - done));
    size_t n = f.read(buf + done, chunk);
    if (n == 0) break;
    done += n;
  }
  f.close();
  return done > 0;
}

struct D88Item { String name; int kind; };
std::vector<D88Item> d88Items;

static void listD88Files(const String &path, bool canGoUp) {
  d88Items.clear();
  File dir = SD.open(path.c_str());
  if (!dir) return;
  std::vector<D88Item> dirs, files;
  File e;
  while ((e = dir.openNextFile())) {
    String name = e.name();
    int s = name.lastIndexOf('/');
    if (s >= 0) name = name.substring(s + 1);
    if (name.length() && name[0] != '.' && name != "System Volume Information") {
      if (e.isDirectory()) dirs.push_back({ name, 1 });
      else {
        String upper = name; upper.toUpperCase();
        if (upper.endsWith(".D88") || isRawImage(upper)) files.push_back({ name, 2 });
      }
    }
    e.close();
  }
  dir.close();
  auto byName = [](const D88Item &a, const D88Item &b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; };
  std::sort(dirs.begin(), dirs.end(), byName);
  std::sort(files.begin(), files.end(), byName);
  if (canGoUp) d88Items.push_back({ "..", 0 });
  d88Items.insert(d88Items.end(), dirs.begin(), dirs.end());
  d88Items.insert(d88Items.end(), files.begin(), files.end());
}

static int listD88Images(const String &path, D88Image *out, int max) {
  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) return 0;
  if (isRawImage(path)) {
    out[0].ofs = 0; out[0].size = f.size(); out[0].name[0] = 0;
    f.close();
    return max > 0 ? 1 : 0;
  }
  uint32_t total = f.size(), o = 0;
  int n = 0;
  uint8_t h[0x20];
  while (n < max && o + 0x2B0 <= total) {
    if (!f.seek(o) || f.read(h, 0x20) != 0x20) break;
    uint32_t sz = h[0x1C] | (h[0x1D] << 8) | (h[0x1E] << 16) | ((uint32_t)h[0x1F] << 24);
    if (sz < 0x2B0 || o + sz > total) {
      if (n > 0) break;
      sz = total - o;
    }
    out[n].ofs = o;
    out[n].size = sz;
    for (int i = 0; i < 17; i++) out[n].name[i] = (h[i] >= 0x20 && h[i] < 0x7F) ? (char)h[i] : (h[i] ? '?' : 0);
    out[n].name[17] = 0;
    n++;
    o += sz;
  }
  f.close();
  return n;
}

static bool loadD88ToDrive(int driveNo, const String &path, int image) {
  X1Drive &drv = x1_drives[driveNo];
  if (!drv.buffer) return false;
  D88Image imgs[D88_MAX_IMAGES];
  int n = listD88Images(path, imgs, D88_MAX_IMAGES);
  if (image < 0 || image >= n) return false;
  uint32_t sz = imgs[image].size;
  const bool raw = isRawImage(path);
  const uint32_t at = raw ? X1_D88_MAX / 2 : 0;
  if (sz > X1_D88_MAX - at) { Serial.printf("disk image too large: %u bytes\n", (unsigned)sz); return false; }
  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) return false;
  f.seek(imgs[image].ofs);
  uint32_t done = 0;
  while (done < sz) {
    size_t r = f.read(drv.buffer + at + done, min((size_t)4096, (size_t)(sz - done)));
    if (r == 0) break;
    done += r;
  }
  f.close();
  if (raw) {
    done = x1RawToD88(drv.buffer, X1_D88_MAX, drv.buffer + at, done);
    if (!done) { Serial.printf("raw image size not supported: %u bytes\n", (unsigned)sz); return false; }
  }
  bool ok = x1LoadD88(drv, drv.buffer, done);
  driveRaw[driveNo] = raw && ok;
  if (ok) {
    drive_path[driveNo] = path;
    driveOfs[driveNo] = imgs[image].ofs;
    int tracks = 0, sectors = 0;
    for (int t = 0; t < X1_D88_TRACKS; t++) if (drv.trk_nsec[t]) { tracks++; sectors += drv.trk_nsec[t]; }
    Serial.printf("D88 %s [%d/%d]: %u bytes, %d tracks, %d sectors%s\n", path.c_str(), image + 1, n, (unsigned)done, tracks, sectors,
                  drv.protect ? " (write protected)" : "");
  } else drive_path[driveNo] = "";
  return ok;
}

static void writeBackD88(int driveNo) {
  if (drive_path[driveNo] == "") { flashStatus("NO D88 LOADED", 1500); if (isPaused) showStatus("NO DISK"); return; }
  const X1Drive &drv = x1_drives[driveNo];
  if (!drv.has_d88_header || drv.size == 0) { flashStatus("D88 WRITEBACK FAILED", 1500); return; }
  File f = SD.open(drive_path[driveNo].c_str(), "r+");
  bool ok = (bool)f;
  if (ok && driveRaw[driveNo]) {
    for (int t = 0; t < X1_D88_TRACKS && ok; t++) {
      uint32_t pos = drv.trk_ofs[t];
      for (int s = 0; s < drv.trk_nsec[t] && ok; s++) {
        const uint8_t *h = drv.buffer + pos;
        uint32_t secSize = h[14] | (h[15] << 8);
        int r = h[2];
        if (secSize == 256 && r >= 1 && r <= 16)
          ok = f.seek(((uint32_t)t * 16 + (r - 1)) * 256) && f.write(h + 16, 256) == 256;
        pos += 16 + secSize;
      }
    }
  } else if (ok) {
    ok = f.seek(driveOfs[driveNo]) && f.write(drv.buffer, drv.size) == drv.size;
  }
  if (f) f.close();
  flashStatus(ok ? "D88 WRITTEN BACK OK" : "D88 WRITEBACK FAILED", 1500);
  if (isPaused) showStatus(ok ? "D88 WRITTEN BACK" : "D88 WRITE BACK FAILED");
}
static const char *ITEM_NONE = "<NONE>";

static String selectD88File(const char *title, const String &current) {
  const String base = "/X1";
  String sub = "", want = current;
  int slash = current.lastIndexOf('/');
  if (current != ITEM_NONE && slash >= 0) { sub = current.substring(0, slash); want = current.substring(slash + 1); }
  menuPalette();
  canvas->selectFont(&fabgl::FONT_8x16);
  auto kb = PS2Controller.keyboard();
  const int visible = 20;

  for (;;) {
    String path = base + (sub == "" ? "" : "/" + sub);
    listD88Files(path, sub != "");
    if (sub == "") d88Items.insert(d88Items.begin(), { String(ITEM_NONE), 3 });
    else if (d88Items.empty()) { sub = ""; want = ""; continue; }
    int cursor = 0;
    for (int i = 0; i < (int)d88Items.size(); i++)
      if (d88Items[i].name == want) { cursor = i; break; }
    bool isBreak = false, isE0 = false, changeDir = false;
    String result;

    int shownTop = -1, shownCursor = -1;
    auto drawItem = [&](int idx, int top) {
      int y = 72 + (idx - top) * 16;
      canvas->setBrushColor(fabgl::Color::Blue);
      canvas->fillRectangle(8, y, 639, y + 15);
      if (idx < 0 || idx >= (int)d88Items.size()) return;
      const D88Item &it = d88Items[idx];
      String label = (it.kind == 2 || it.kind == 3) ? it.name : it.name + "/";
      canvas->setPenColor(idx == cursor ? fabgl::Color::BrightYellow
                          : (it.kind == 2 || it.kind == 3) ? fabgl::Color::BrightWhite : fabgl::Color::BrightCyan);
      canvas->drawText(16, y, label.c_str());
    };
    int listTop = 0;
    auto draw = [&]() {
      if (cursor < listTop) listTop = cursor;
      if (cursor >= listTop + visible) listTop = cursor - visible + 1;
      int top = listTop;
      if (shownTop < 0) {
        canvas->setBrushColor(fabgl::Color::Blue);
        canvas->clear();
        canvas->setPenColor(fabgl::Color::White);
        canvas->drawText(8, 16, title);
        canvas->drawText(8, 32, "UP/DOWN:SELECT ENTER:OK BS:UP ESC:CANCEL");
        String shown = path + "/";
        if (shown.length() > 76) shown = "..." + shown.substring(shown.length() - 73);
        canvas->setPenColor(fabgl::Color::Cyan);
        canvas->drawText(8, 48, shown.c_str());
      }
      if (shownTop < 0 || top != shownTop) {
        for (int i = 0; i < visible; i++) drawItem(top + i, top);
      } else if (cursor != shownCursor) {
        drawItem(shownCursor, top);
        drawItem(cursor, top);
      }
      shownTop = top;
      shownCursor = cursor;
    };
    draw();

    while (!changeDir) {
      int jsc = 0;
      if (!kb->scancodeAvailable() && (jsc = joyMenuScancode(&isE0)) != 0) isBreak = false;
      if (jsc || kb->scancodeAvailable()) {
        int sc = jsc ? jsc : kb->getNextScancode(0);
        if (sc == 0xF0) { isBreak = true; continue; }
        if (sc == 0xE0) { isE0 = true; continue; }
        if (!isBreak) {
          int n = (int)d88Items.size();
          if (sc == 0x75 && isE0) { if (cursor > 0) cursor--; draw(); }
          else if (sc == 0x72 && isE0) { if (cursor < n - 1) cursor++; draw(); }
          else if (sc == 0x7D && isE0) { cursor = cursor - visible < 0 ? 0 : cursor - visible; draw(); }
          else if (sc == 0x7A && isE0) { cursor = cursor + visible >= n ? n - 1 : cursor + visible; draw(); }
          else if ((sc == 0x66 && sub != "") || (sc == 0x5A && d88Items[cursor].kind == 0)) {
            int p = sub.lastIndexOf('/');
            want = p >= 0 ? sub.substring(p + 1) : sub;
            sub = p >= 0 ? sub.substring(0, p) : "";
            changeDir = true;
          } else if (sc == 0x5A && d88Items[cursor].kind == 1) {
            sub = sub == "" ? d88Items[cursor].name : sub + "/" + d88Items[cursor].name;
            want = "";
            changeDir = true;
          } else if (sc == 0x5A) {
            const D88Item &it = d88Items[cursor];
            return (it.kind == 3 || sub == "") ? it.name : sub + "/" + it.name;
          } else if (sc == 0x76) {
            return "";
          }
        }
        isBreak = false; isE0 = false;
      }
      vTaskDelay(1);
    }
  }
}

static int selectD88Image(const char *title, const D88Image *imgs, int n, int current) {
  auto kb = PS2Controller.keyboard();
  int cursor = current < 0 || current >= n ? 0 : current;
  bool isBreak = false, isE0 = false, redraw = true;
  int shownCursor = -1;
  menuPalette();
  canvas->selectFont(&fabgl::FONT_8x16);
  auto drawLine = [&](int i) {
    char line[48];
    snprintf(line, sizeof(line), "%2d: %s", i + 1, imgs[i].name[0] ? imgs[i].name : "(NO NAME)");
    canvas->setBrushColor(fabgl::Color::Blue);
    canvas->fillRectangle(8, 72 + i * 16, 639, 72 + i * 16 + 15);
    canvas->setPenColor(i == cursor ? fabgl::Color::BrightYellow : fabgl::Color::BrightWhite);
    canvas->drawText(16, 72 + i * 16, line);
  };
  for (;;) {
    if (redraw) {
      if (shownCursor < 0) {
        canvas->setBrushColor(fabgl::Color::Blue);
        canvas->clear();
        canvas->setPenColor(fabgl::Color::White);
        canvas->drawText(8, 16, title);
        canvas->drawText(8, 32, "THIS FILE HAS SEVERAL DISKS. UP/DOWN:SELECT ENTER:OK ESC:CANCEL");
        for (int i = 0; i < n; i++) drawLine(i);
      } else if (shownCursor != cursor) {
        drawLine(shownCursor);
        drawLine(cursor);
      }
      shownCursor = cursor;
      redraw = false;
    }
    int jsc = 0;
    if (!kb->scancodeAvailable() && (jsc = joyMenuScancode(&isE0)) != 0) isBreak = false;
    if (jsc || kb->scancodeAvailable()) {
      int sc = jsc ? jsc : kb->getNextScancode(0);
      if (sc == 0xF0) { isBreak = true; continue; }
      if (sc == 0xE0) { isE0 = true; continue; }
      if (!isBreak) {
        if (sc == 0x75 && isE0) { if (cursor > 0) cursor--; redraw = true; }
        else if (sc == 0x72 && isE0) { if (cursor < n - 1) cursor++; redraw = true; }
        else if (sc == 0x5A) return cursor;
        else if (sc == 0x76) return -1;
      }
      isBreak = false; isE0 = false;
    }
    vTaskDelay(1);
  }
}

static String bootDisk[NUM_DRIVES];
static int bootImage[NUM_DRIVES];
static String lastChosen;
static String dirOf(const String &f) { int s = f.lastIndexOf('/'); return s >= 0 ? f.substring(0, s) : String(""); }

static bool chooseDisk(int d, String &file, int &image, const String &otherFile, int otherImage) {
  String recent = lastChosen != "" ? lastChosen : otherFile;
  String start = file;
  if (recent != "" && (file == "" || dirOf(file) != dirOf(recent))) start = recent;
  String sel = selectD88File(d ? "X1  SELECT DISK FOR FD1" : "X1  SELECT DISK FOR FD0",
                             start == "" ? String(ITEM_NONE) : start);
  if (sel == "") return false;
  if (sel == ITEM_NONE) { file = ""; image = 0; return true; }
  D88Image imgs[D88_MAX_IMAGES];
  int n = listD88Images("/X1/" + sel, imgs, D88_MAX_IMAGES);
  int img = 0;
  if (n > 1) {
    int def = (sel == file) ? image : (sel == otherFile) ? otherImage + (d == 1 ? 1 : -1) : (d == 1 ? 1 : 0);
    if (def < 0) def = 0;
    if (def >= n) def = n - 1;
    img = selectD88Image(sel.c_str(), imgs, n, def);
    if (img < 0) return false;
  }
  file = sel;
  image = img;
  lastChosen = sel;
  return true;
}

static char menuShown[9][100];
static bool menuValid = false;
static void bootMenuInvalidate() { menuValid = false; }

static void bootMenuDraw(int cursor, const char *msg, int capture = -1) {
  const int X0 = 40, X1 = 599, Y0 = 72, DY = 26;
  canvas->selectFont(&fabgl::FONT_8x16);
  if (!menuValid) {
    menuPalette();
    canvas->setBrushColor(fabgl::Color::Blue);
    canvas->clear();
    canvas->setPenColor(fabgl::Color::BrightWhite);
    canvas->drawText(X0, 28, "SHARP X1turbo BOOT MENU");
    memset(menuShown, 0, sizeof(menuShown));
    menuValid = true;
  }
  for (int i = 0; i < 7; i++) {
    char line[96];
    switch (i) {
      case 0: case 1: {
        D88Image imgs[D88_MAX_IMAGES];
        int n = bootDisk[i] == "" ? 0 : listD88Images("/X1/" + bootDisk[i], imgs, D88_MAX_IMAGES);
        if (n > 1) snprintf(line, sizeof(line), "Fd%d     : %.50s [%d/%d]", i, bootDisk[i].c_str(), bootImage[i] + 1, n);
        else snprintf(line, sizeof(line), "Fd%d     : %.60s", i, bootDisk[i].c_str());
        break;
      }
      case 2: snprintf(line, sizeof(line), "JoyToKey: %s", joyToKeyOn ? "ON" : "OFF"); break;
      case 3: case 4: case 5:
        if (i == capture) snprintf(line, sizeof(line), "Joy B%d  : PRESS A KEY...", i - 2);
        else snprintf(line, sizeof(line), "Joy B%d  : %s", i - 2, x1KeyName(joyButtonKey[i - 3]));
        break;
      default: snprintf(line, sizeof(line), "Run"); break;
    }
    bool sel = (i == cursor);
    char shown[100];
    snprintf(shown, sizeof(shown), "%c%s", sel ? (i == capture ? 'C' : 'S') : 'N', line);
    if (strcmp(shown, menuShown[i]) == 0) continue;
    strcpy(menuShown[i], shown);
    int y = Y0 + i * DY;
    canvas->setBrushColor(!sel ? fabgl::Color::Blue : i == capture ? fabgl::Color::BrightYellow : fabgl::Color::BrightWhite);
    canvas->fillRectangle(X0 - 4, y - 5, X1, y + 19);
    canvas->setPenColor(sel ? fabgl::Color::Blue : fabgl::Color::BrightWhite);
    canvas->drawText(X0, y, line);
  }
  const char *help = capture >= 0 ? "PRESS THE KEY FOR THIS JOYSTICK BUTTON (F6-F12: CANCEL)"
                                  : "UP/DOWN: SELECT   ENTER: CHANGE / RUN   (JOYTOKEY: NUMPAD 1-9)";
  const char *texts[2] = { help, msg ? msg : "" };
  for (int k = 0; k < 2; k++) {
    if (strcmp(texts[k], menuShown[7 + k]) == 0) continue;
    snprintf(menuShown[7 + k], sizeof(menuShown[0]), "%s", texts[k]);
    int y = k ? 400 - 40 : 400 - 68;
    canvas->setBrushColor(fabgl::Color::Blue);
    canvas->fillRectangle(X0 - 4, y - 2, 639, y + 17);
    canvas->setPenColor(k ? fabgl::Color::BrightWhite : fabgl::Color::BrightCyan);
    canvas->drawText(X0, y, texts[k]);
  }
}

static void x1BootMenu() {
  auto kb = PS2Controller.keyboard();
  const int NITEMS = 7;
  int pos = 0, capture = -1;
  const char *msg = nullptr;
  bool isBreak = false, isE0 = false, redraw = true, joyChanged = false;
  bootMenuInvalidate();
  for (;;) {
    if (redraw) { bootMenuDraw(pos, msg, capture); redraw = false; }
    int jsc = 0;
    if (capture < 0 && !kb->scancodeAvailable() && (jsc = joyMenuScancode(&isE0)) != 0) isBreak = false;
    if (jsc || kb->scancodeAvailable()) {
      int sc = jsc ? jsc : kb->getNextScancode(0);
      if (sc == 0xF0) { isBreak = true; continue; }
      if (sc == 0xE0) { isE0 = true; continue; }
      if (!isBreak && capture >= 0) {
        for (int i = 0; i < keyMapCount; i++)
          if (keyMapTable[i].ps2 == sc && keyMapTable[i].ext == isE0) {
            joyButtonKey[capture - 3] = keyMapTable[i].key;
            joyChanged = true;
            break;
          }
        capture = -1;
        redraw = true;
      } else if (!isBreak) {
        if (sc == 0x75 && isE0)      { pos = (pos + NITEMS - 1) % NITEMS; redraw = true; }
        else if (sc == 0x72 && isE0) { pos = (pos + 1) % NITEMS; redraw = true; }
        else if (sc == 0x5A && pos >= 3 && pos <= 5) { capture = pos; redraw = true; }
        else if (sc == 0x5A && pos == 2) { joyToKeyOn = !joyToKeyOn; joyChanged = true; redraw = true; }
        else if (sc == 0x5A) {
          msg = nullptr;
          if (pos == 0 || pos == 1) {
            bootMenuInvalidate();
            if (chooseDisk(pos, bootDisk[pos], bootImage[pos], bootDisk[pos ^ 1], bootImage[pos ^ 1]) && pos == 0 && bootDisk[0] != "" && bootDisk[1] == "") {
              D88Image imgs[D88_MAX_IMAGES];
              if (bootImage[0] + 1 < listD88Images("/X1/" + bootDisk[0], imgs, D88_MAX_IMAGES)) {
                bootDisk[1] = bootDisk[0];
                bootImage[1] = bootImage[0] + 1;
              }
            }
          } else {
            bootMenuDraw(pos, "LOADING...");
            for (int d = 0; d < NUM_DRIVES && !msg; d++)
              if (bootDisk[d] != "" && !loadD88ToDrive(d, "/X1/" + bootDisk[d], bootImage[d])) msg = d ? "FD1 LOAD ERROR" : "FD0 LOAD ERROR";
            if (!msg) break;
          }
          redraw = true;
        }
      }
      isBreak = false; isE0 = false;
    }
    vTaskDelay(1);
  }
  if (joyChanged) saveJoyKeys();
  canvas->setBrushColor(fabgl::Color::Black);
  canvas->clear();
}
static void showStatus(const char *msg) {
  x1VideoUnmapRows();
  uint8_t **rows = DisplayController.rowTable();
  const fabgl::FontInfo &fnt = fabgl::FONT_8x16;
  for (int r = 0; r < 20; r++) memset(rows[380 + r], 0x11, X1_ROW_BYTES);
  for (int i = 0; msg[i] && i < 79; i++) {
    const uint8_t *g = fnt.data + (uint8_t)msg[i] * 16;
    for (int r = 0; r < 16; r++) {
      uint8_t *d = rows[382 + r] + 2 + i * 4;
      uint8_t bits = g[r];
      for (int k = 0; k < 4; k++) {
        uint8_t hi = (bits & (0x80 >> (2 * k))) ? 7 : 1, lo = (bits & (0x40 >> (2 * k))) ? 7 : 1;
        d[k] = (uint8_t)((hi << 4) | lo);
      }
    }
  }
}

static void driveDisk(int d, String &file, int &image) {
  file = drive_path[d].startsWith("/X1/") ? drive_path[d].substring(4) : String("");
  image = 0;
  if (file != "") {
    D88Image imgs[D88_MAX_IMAGES];
    int n = listD88Images(drive_path[d], imgs, D88_MAX_IMAGES);
    for (int i = 0; i < n; i++) if (imgs[i].ofs == driveOfs[d]) image = i;
  }
}

static void swapDisk(int d) {
  String file, other;
  int image, otherImage;
  driveDisk(d, file, image);
  driveDisk(d ^ 1, other, otherImage);
  bool err = false;
  if (chooseDisk(d, file, image, other, otherImage)) {
    if (file == "") { x1_drives[d].inserted = false; drive_path[d] = ""; }
    else if (!loadD88ToDrive(d, "/X1/" + file, image)) err = true;
  }
  canvas->setBrushColor(fabgl::Color::Black);
  canvas->clear();
  canvas->waitCompletion();
  x1VideoResume();
  showStatus(err ? "D88 LOAD ERROR" : PAUSE_MSG);
}
#define VGA_START_LINE 380
static void vgaWaitFrame(int64_t frameStart) {
  for (;;) {
    int64_t el = esp_timer_get_time() - frameStart;
    if (el >= X1_FRAME_US + 1500) return;
    if (el < X1_FRAME_US / 2) { vTaskDelay(pdMS_TO_TICKS((X1_FRAME_US / 2 - el) / 1000 + 1)); continue; }
    int s = X1VGA16::scanLine();
    if (s >= VGA_START_LINE + 4 || s <= 2) return;
    int us = (VGA_START_LINE + 4 - s) * 32;
    if (us > 2500) vTaskDelay(pdMS_TO_TICKS(us / 1000 - 1));
  }
}

#ifdef USE_SERIAL_DEBUG
#include "esp_freertos_hooks.h"
static volatile uint32_t core1IdleCyc = 0;
static uint32_t core1IdleLast = 0;
static bool IRAM_ATTR core1IdleHook() {
  uint32_t now = xthal_get_ccount(), d = now - core1IdleLast;
  core1IdleLast = now;
  if (d < 1200) core1IdleCyc += d;
  return false;
}
#endif

void cpuTask(void *pvParameters) {
  const int64_t frameTimeUs = X1_FRAME_US;
  int64_t nextFrameTime = esp_timer_get_time();
  uint32_t usAccum = 0;
  int cpuDebt = 0;
  bool renderSkipped = false;
  bool noWait = false, prevNoWait = false;

#ifdef USE_SERIAL_DEBUG
  int64_t dbgMin = INT64_MAX, dbgMax = 0, dbgSum = 0;
  int dbgOverBudget = 0;
  int dbgFrameCnt = 0;
  int64_t dbgWinStart = 0, dbgIdle = 0, dbgRendMax = 0, dbgRestMax = 0;
  int dbgHist[4] = { 0, 0, 0, 0 };
  int dbgSkipFix = 0, dbgCpuBound = 0, dbgFull = 0, dbgRendSkipped = 0;
  uint32_t dbgLinesMax = 0;
#endif
#ifdef X1_PROFILE
  uint64_t profZ80 = 0, profTick = 0, profEnd = 0;
#ifdef USE_SERIAL_DEBUG
  x1MemBench();
  nextFrameTime = esp_timer_get_time();
#endif
#endif

  while (1) {
    if (isPaused) {
      vTaskDelay(pdMS_TO_TICKS(10));
      nextFrameTime = esp_timer_get_time();
      continue;
    }
    if (resetRequest) {
      resetRequest = false;
      x1Reset();
      ResetZ80(&cpu);
#if X1_OPM_BOARD
      opmResetRequest = true;
#endif
    }

#ifdef USE_SERIAL_DEBUG
    int64_t frameStart = esp_timer_get_time();
    if (dbgFrameCnt == 0) dbgWinStart = frameStart;
#ifdef X1_PROFILE
    const uint32_t dbgRend0 = x1_prof_render, dbgLines0 = x1_lines_drawn;
#endif
#endif

    updateJoystick(prevNoWait);
    joyToKeys();
    prevNoWait = noWait;
    x1FrameStart();
    const int lines = x1_lines_frame, cyc = x1_cyc_line;
    x1_render_skip = !renderSkipped && esp_timer_get_time() > nextFrameTime + 1000;
    renderSkipped = x1_render_skip;
    nextFrameTime += frameTimeUs;
    for (int i = 0; i < lines; i++) {
      int run = cyc - cpuDebt - x1DmaTakeCycles();
#ifdef X1_PROFILE
      uint32_t pc0 = x1Ccount();
      if (run > 0) cpuDebt = -ExecZ80(&cpu, run); else cpuDebt = -run;
      uint32_t pc1 = x1Ccount();
      x1ScanlineTick(i);
#if X1_OPM_BOARD
      opmEmuClock += cyc;
#endif
      profZ80  += pc1 - pc0;
      profTick += x1Ccount() - pc1;
#else
      if (run > 0) cpuDebt = -ExecZ80(&cpu, run); else cpuDebt = -run;
      x1ScanlineTick(i);
#if X1_OPM_BOARD
      opmEmuClock += cyc;
#endif
#endif

      if (i == x1_disp_lines) {
        frameReady = true;
      }
      if (x1IrqPending() && (cpu.IFF & IFF_1)) {
        x1IrqAck();
        IntZ80(&cpu, x1IrqVector());
      }
    }
#ifdef X1_PROFILE
    uint32_t pe0 = x1Ccount();
#endif
    x1FrameTick();

    usAccum += (uint32_t)lines * cyc / 4;
    while (usAccum >= 1000) { x1FdcTick1ms(); usAccum -= 1000; }
#ifdef X1_PROFILE
    profEnd += x1Ccount() - pe0;
#endif

#ifdef USE_SERIAL_DEBUG
    int64_t elapsed = esp_timer_get_time() - frameStart;
    if (elapsed < dbgMin) dbgMin = elapsed;
    if (elapsed > dbgMax) dbgMax = elapsed;
    dbgSum += elapsed;
    if (elapsed > frameTimeUs) dbgOverBudget++;
    if (x1_render_skip) dbgRendSkipped++;
    dbgHist[elapsed <= frameTimeUs ? 0 : elapsed <= frameTimeUs * 3 / 2 ? 1 : elapsed <= frameTimeUs * 2 ? 2 : 3]++;
#ifdef X1_PROFILE
    {
      const int64_t rendUs = (int64_t)((uint32_t)(x1_prof_render - dbgRend0) / 240u);
      const int64_t restUs = elapsed - rendUs;
      if (rendUs > dbgRendMax) dbgRendMax = rendUs;
      if (restUs > dbgRestMax) dbgRestMax = restUs;
      if (elapsed > frameTimeUs) { if (restUs <= frameTimeUs) dbgSkipFix++; else dbgCpuBound++; }
      const uint32_t ln = x1_lines_drawn - dbgLines0;
      if (ln > dbgLinesMax) dbgLinesMax = ln;
      if (ln >= (uint32_t)x1_disp_lines) dbgFull++;
    }
#endif
    if (++dbgFrameCnt >= 300) {
      {
        const int64_t wall = esp_timer_get_time() - dbgWinStart;
        const uint32_t c1 = core1IdleCyc; core1IdleCyc = 0;
        Serial.printf("SPEED: %d%% of real (%d frames in %lld ms) | frame <=1x:%d <=1.5x:%d <=2x:%d >2x:%d | max render=%lldus rest=%lldus | over: renderSkipFixes=%d cpuBound=%d | fullRedraw=%d maxLines=%u renderSkipped=%d | idle avg=%lldus core1 idle=%d%%\n",
                      (int)((int64_t)dbgFrameCnt * frameTimeUs * 100 / (wall > 0 ? wall : 1)), dbgFrameCnt, (long long)(wall / 1000),
                      dbgHist[0], dbgHist[1], dbgHist[2], dbgHist[3], (long long)dbgRendMax, (long long)dbgRestMax,
                      dbgSkipFix, dbgCpuBound, dbgFull, (unsigned)dbgLinesMax, dbgRendSkipped, (long long)(dbgIdle / dbgFrameCnt),
                      (int)((uint64_t)c1 * 100 / ((uint64_t)(wall > 0 ? wall : 1) * 240)));
        dbgIdle = dbgRendMax = dbgRestMax = 0;
        dbgHist[0] = dbgHist[1] = dbgHist[2] = dbgHist[3] = 0;
        dbgSkipFix = dbgCpuBound = dbgFull = dbgRendSkipped = 0;
        dbgLinesMax = 0;
      }
      Serial.printf("FRAME us: min=%lld max=%lld avg=%lld overBudget=%d/%d (budget=%lld)\n",
                    (long long)dbgMin, (long long)dbgMax, (long long)(dbgSum / dbgFrameCnt),
                    dbgOverBudget, dbgFrameCnt, (long long)frameTimeUs);
#ifdef X1_PROFILE
      {
        const double div = 240.0 * dbgFrameCnt;
        double z80 = profZ80 / div, tick = profTick / div, rend = x1_prof_render / div, end = profEnd / div;
        double avg = (double)dbgSum / dbgFrameCnt;
        Serial.printf("PROF us/frame: z80=%.0f render=%.0f (txt=%.0f rd=%.0f bmp=%.0f) tickOther=%.0f frameEnd=%.0f unaccounted=%.0f | us/cyc=%.3f %s %dx%d hres=%d 1FD0=%02X pri=%02X lines=%u/%d txtSame=%u\n",
                      z80, rend, x1_prof_txt / div, x1_prof_rd / div, x1_prof_bmp / div,
                      tick - rend, end, avg - (z80 + tick + end),
                      z80 / ((double)x1_lines_frame * x1_cyc_line), x1_hireso ? "24kHz" : "15kHz", x1_cyc_line, x1_lines_frame,
                      (int)hres_mode, x1_mode1, priority_reg, (unsigned)(x1_lines_drawn / dbgFrameCnt), x1_disp_lines,
                      (unsigned)(x1_lines_txtsame / dbgFrameCnt));
        profZ80 = profTick = profEnd = 0;
        x1_prof_render = 0;
        x1_prof_rd = x1_prof_bmp = x1_prof_txt = 0;
        x1_lines_drawn = 0;
        x1_lines_txtsame = 0;
      }
#endif
#ifdef X1_RAM_STATS
      {
        Serial.printf("RAM hits/60f: p0=%u p1=%u p2=%u p3=%u\n",
                      (unsigned)x1_ram_hits[0], (unsigned)x1_ram_hits[1],
                      (unsigned)x1_ram_hits[2], (unsigned)x1_ram_hits[3]);
        for (int i = 0; i < X1_RAM_PAGES; i++) x1_ram_hits[i] = 0;
      }
#endif
      dbgMin = INT64_MAX; dbgMax = 0; dbgSum = 0; dbgOverBudget = 0; dbgFrameCnt = 0;
    }
#endif

    int64_t now = esp_timer_get_time();
    int64_t waitTime = nextFrameTime - now;
    noWait = waitTime <= 0;
    if (waitTime > 0) {
      vgaWaitFrame(nextFrameTime - frameTimeUs);
      nextFrameTime = esp_timer_get_time();
#ifdef USE_SERIAL_DEBUG
      dbgIdle += nextFrameTime - now;
#endif
    } else if (waitTime < -2 * frameTimeUs) {
      nextFrameTime = now;
    }
  }
}

#ifndef BOARD_HAS_PSRAM
bool psramEnable() {
  if (esp_spiram_init() != ESP_OK) return false;
  esp_spiram_init_cache();
  if (!esp_spiram_test()) return false;
  if (esp_spiram_add_to_heapalloc() != ESP_OK) return false;
  heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
  return true;
}
#endif

void setup() {
  Serial.begin(115200);
#ifndef BOARD_HAS_PSRAM
  Serial.printf("PSRAM: %s\n", psramEnable() ? "enabled by the sketch" : "init FAILED");
#endif
  setCpuFrequencyMhz(240);

  disableCore0WDT();
  disableCore1WDT();
  rtc_wdt_protect_off();
  rtc_wdt_disable();
  esp_task_wdt_delete(NULL);
  delay(100);

#ifdef USE_SERIAL_DEBUG
  #define HEAPLOG(tag) Serial.printf("HEAP[%-10s] free=%6u largest=%6u\n", (tag), \
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL), \
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL))
#else
  #define HEAPLOG(tag)
#endif
  HEAPLOG("boot");

  PS2Controller.begin(PS2Preset::KeyboardPort0_MousePort1, fabgl::KbdMode::NoVirtualKeys);
  HEAPLOG("after PS2");
  PS2Controller.keyboard()->begin(GPIO_NUM_33, GPIO_NUM_32, false, false);
  PS2Controller.keyboard()->setLEDs(false, false, false);
  Serial1.begin(115200, SERIAL_8N1, 39, -1);

  HEAPLOG("before VGA");
  DisplayController.begin();
  {
    static char mode[96];
    const int vfp = 450 + (X1_VGA_VTOTAL - 524) / 2;
    snprintf(mode, sizeof(mode), "\"640x400\" 25.175 640 656 752 800 400 %d %d %d -HSync -VSync", vfp, vfp + 2, X1_VGA_VTOTAL);
    DisplayController.setResolution(mode, -1, -1, false);
    Serial.printf("VGA: %s (%.2fHz)\n", mode, 25175000.0 / 800 / X1_VGA_VTOTAL);
  }
  HEAPLOG("after VGA");

  x1_ipl_rom = (uint8_t*)ps_malloc(X1_IPL_SIZE);
  x1_cgrom   = (uint8_t*)heap_caps_malloc(0x800, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  x1_tvram   = (uint8_t*)heap_caps_malloc(0x800, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  x1_avram   = (uint8_t*)heap_caps_malloc(0x800, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  x1_kvram   = (uint8_t*)heap_caps_malloc(0x800, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  for (int i = 0; i < 3; i++)
    x1_pcg[i] = (uint8_t*)heap_caps_malloc(0x800, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  x1_gvram   = (uint8_t*)ps_malloc(GVRAM_ALLOC_SZ);
  x1_kanji   = (uint8_t*)ps_malloc(X1_KANJI_SIZE);
  x1_extram  = (uint8_t*)ps_malloc(X1_EXTRAM_BANKS * 0x8000);
  if (x1_ipl_rom) memset(x1_ipl_rom, 0xFF, X1_IPL_SIZE);
  if (x1_kanji) memset(x1_kanji, 0, X1_KANJI_SIZE);
  if (x1_extram) memset(x1_extram, 0, X1_EXTRAM_BANKS * 0x8000);

  {
    const size_t X1_INTERNAL_RESERVE = 24 * 1024;
    const int order[X1_RAM_PAGES] = { 3, 0, 1, 2 };
    for (int i = 0; i < X1_RAM_PAGES; i++) {
      int pg = order[i];
      uint8_t *p = nullptr;
      if (heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL) >= X1_RAM_PAGE_SZ + X1_INTERNAL_RESERVE) {
        p = (uint8_t*)heap_caps_malloc(X1_RAM_PAGE_SZ, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
      }
      if (!p) p = (uint8_t*)ps_malloc(X1_RAM_PAGE_SZ);
      x1_ram_page[pg] = p;
    }
  }
  HEAPLOG("after RAM");
  bool ramOK = true;
  for (int i = 0; i < X1_RAM_PAGES; i++) if (!x1_ram_page[i]) ramOK = false;

  if (!ramOK || !x1_ipl_rom || !x1_cgrom || !x1_tvram || !x1_avram || !x1_kvram || !x1_pcg[2] || !x1_gvram || !x1_kanji || !x1_extram) {
    Serial.println("Memory allocation FAILED!");
    while (1) delay(1000);
  }

#ifdef USE_SERIAL_DEBUG
  Serial.printf("MEM: ram0=%p ram1=%p ram2=%p ram3=%p ipl=%p cgrom=%p tvram=%p avram=%p pcg0=%p gvram=%p\n",
                x1_ram_page[0], x1_ram_page[1], x1_ram_page[2], x1_ram_page[3],
                x1_ipl_rom, x1_cgrom, x1_tvram, x1_avram, x1_pcg[0], x1_gvram);
  {
    char kind[X1_RAM_PAGES + 1];
    for (int i = 0; i < X1_RAM_PAGES; i++)
      kind[i] = ((uintptr_t)x1_ram_page[i] >= 0x3FFAE000u) ? 'I' : 'P';
    kind[X1_RAM_PAGES] = 0;
    Serial.printf("RAM pages 0..3: %s (I=内部 P=PSRAM)\n", kind);
  }
  Serial.printf("HEAP: internal_free=%u internal_largest=%u psram_free=%u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif
  for (int i = 0; i < X1_RAM_PAGES; i++) memset(x1_ram_page[i], 0, X1_RAM_PAGE_SZ);

  for (int i = 0; i < NUM_DRIVES; i++) {
    memset(&x1_drives[i], 0, sizeof(x1_drives[i]));
    x1_drives[i].buffer = (uint8_t*)ps_malloc(X1_D88_MAX);
  }
  x1FdcInit(x1_drives, NUM_DRIVES);

  canvas = new fabgl::Canvas(&DisplayController);
  canvas->setBrushColor(fabgl::Color::Black);
  canvas->clear();
  if (psramFixMissing()) {
    Serial.printf("ESP32 revision %d needs the PSRAM errata fix: set Tools > PSRAM to \"Enabled\" and build again\n", ESP.getChipRevision());
    char msg[48];
    snprintf(msg, sizeof(msg), "ESP32 REV.%d NEEDS THE PSRAM BUG FIX", ESP.getChipRevision());
    canvas->selectFont(&fabgl::FONT_8x16);
    canvas->setPenColor(fabgl::Color::Red);
    canvas->drawText(8, 16, msg);
    canvas->drawText(8, 48, "ARDUINO IDE: TOOLS > PSRAM > \"Enabled\", THEN BUILD AGAIN");
    while (1) delay(1000);
  }

#if X1_OPM_BOARD
  opmQueue = (volatile uint32_t *)heap_caps_malloc(sizeof(uint32_t) * OPM_QUEUE, MALLOC_CAP_SPIRAM);
  if (!opmQueue) opmQueue = (volatile uint32_t *)malloc(sizeof(uint32_t) * OPM_QUEUE);
#endif
  soundGenerator.attach(&psgGen);
  psgGen.enable(true);
  soundGenerator.setVolume(100);
  soundGenerator.play(true);

  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  bool romOK = false;
  if (SD.begin(SD_CS, SPI)) {
    romOK = loadFileToBuf("/X1/BIOS/IPLROM.X1T", x1_ipl_rom, X1_IPL_SIZE) || loadFileToBuf("/X1/IPLROM.X1T", x1_ipl_rom, X1_IPL_SIZE);
    bool fontOK = loadFileToBuf("/X1/BIOS/FNT0808.X1", x1_cgrom, 0x800) || loadFileToBuf("/X1/FNT0808.X1", x1_cgrom, 0x800)
               || loadFileToBuf("/X1/FONT.ROM", x1_cgrom, 0x800) || loadFileToBuf("/X1/BIOS/FONT.ROM", x1_cgrom, 0x800);
    bool ank16OK = loadFileToBuf("/X1/BIOS/FNT0816.X1", x1_kanji, 0x1000) || loadFileToBuf("/X1/FNT0816.X1", x1_kanji, 0x1000);
    bool kanjiOK = loadFileToBuf("/X1/BIOS/FNT1616.X1", x1_kanji + 0x1000, X1_KANJI_SIZE - 0x1000)
                || loadFileToBuf("/X1/FNT1616.X1", x1_kanji + 0x1000, X1_KANJI_SIZE - 0x1000);
    if (kanjiOK) x1KanjiDeinterleave(x1_kanji);
    if (!romOK) Serial.println("IPLROM.X1T not found!");
    if (!fontOK) Serial.println("FNT0808.X1 not found!");
    if (!ank16OK) Serial.println("FNT0816.X1 not found (ANK 8x16 not shown)");
    if (!kanjiOK) Serial.println("FNT1616.X1 not found (kanji not shown)");
    romOK = romOK && fontOK;

    if (romOK) {
      loadJoyKeys();
      x1BootMenu();
    }
  } else {
    Serial.println("SD init failed!");
  }

  if (!romOK) {
    canvas->selectFont(&fabgl::FONT_8x16);
    canvas->setPenColor(fabgl::Color::Red);
    canvas->drawText(8, 16, "IPLROM.X1T / FNT0808.X1 NOT FOUND");
    canvas->drawText(8, 48, "PUT ROMS IN /X1/BIOS/ ON SD CARD");
    while (1) delay(1000);
  }

  DisplayController.processPrimitives();
  canvas->waitCompletion();
  uint8_t **vpRows = DisplayController.rowTable();
  for (int y = 0; y < X1_SCREEN_H; y++) {
    if (!vpRows[y] || ((uintptr_t)vpRows[y] & 3)) {
      Serial.printf("VIEWPORT row %d is NULL or unaligned (%p) - 32bit描画は使えません\n",
                    y, vpRows[y]);
      while (1) delay(1000);
    }
  }
#ifdef USE_SERIAL_DEBUG
  {
    int dup = 0, ext = 0;
    uintptr_t lo = ~(uintptr_t)0, hi = 0;
    for (int y = 0; y < X1_SCREEN_H; y++) {
      uintptr_t p = (uintptr_t)vpRows[y];
      if (p < lo) lo = p;
      if (p > hi) hi = p;
      if (p < 0x3FFAE000u) ext++;
      for (int k = 0; k < y; k++)
        if (vpRows[k] == vpRows[y]) { dup++; break; }
    }
    Serial.printf("VIEWPORT CHECK: rows=%d dup=%d outsideDRAM=%d lo=%p hi=%p (期待:dup=0 outside=0)\n",
                  X1_SCREEN_H, dup, ext, (void*)lo, (void*)hi);
  }
#endif
  x1VideoSetOutput(vpRows);
  x1VideoBuildTables();

  X1Memory mem;
  for (int i = 0; i < X1_RAM_PAGES; i++) mem.ram_page[i] = x1_ram_page[i];
  mem.ipl_rom = x1_ipl_rom;
  mem.cgrom = x1_cgrom;
  mem.tvram = x1_tvram;
  mem.avram = x1_avram;
  mem.kvram = x1_kvram;
  mem.pcg[0] = x1_pcg[0]; mem.pcg[1] = x1_pcg[1]; mem.pcg[2] = x1_pcg[2];
  mem.gvram = x1_gvram;
  mem.kanji = x1_kanji;
  mem.extram = x1_extram;
  x1Init(mem);

  memset(&cpu, 0, sizeof(Z80));
  ResetZ80(&cpu);
  cpu.IPeriod = X1_CYCLES_PER_LINE;

  Serial.println("X1turbo Booting...");
#ifndef USE_SERIAL_DEBUG
  Serial.end();
#endif

#ifdef USE_SERIAL_DEBUG
  core1IdleLast = xthal_get_ccount();
  esp_register_freertos_idle_hook_for_cpu(core1IdleHook, 1);
#endif
  xTaskCreatePinnedToCore(cpuTask, "cpuTask", 8192, NULL, 24, NULL, 0);
  HEAPLOG("end setup");
}

static void updateMouse() {
  auto m = PS2Controller.mouse();
  if (!m) return;
  while (m->deltaAvailable()) {
    MouseDelta d;
    if (!m->getNextDelta(&d, 0)) break;
    x1_mouse_dx += d.deltaX;
    x1_mouse_dy -= d.deltaY;
    x1_mouse_buttons = (d.buttons.left ? 1 : 0) | (d.buttons.right ? 2 : 0);
  }
}

void loop() {
  psgGen.fillBuffer();
  if (isPaused) {
    updateKeyboard();
    vTaskDelay(pdMS_TO_TICKS(10));
    return;
  }

  if (frameReady) {
    frameReady = false;
    updateKeyboard();
    updateMouse();

#ifdef USE_SERIAL_DEBUG
    static int dbgFrameCnt = 0;
    if (++dbgFrameCnt >= 300) {
      dbgFrameCnt = 0;
      char dbg[96];
      x1FdcDebugDump(dbg, sizeof(dbg));
      Serial.print("FDC ");
      Serial.println(dbg);
      Serial.print("TVRAM[0..15]: ");
      for (int i = 0; i < 16; i++) {
        Serial.printf("%02X ", M.tvram[i]);
      }
      Serial.println();
      Serial.printf("VID R1=%d R6=%d R9=%d R12/13=%04X pal B/R/G=%02X/%02X/%02X pri=%02X hres=%d portC=%02X joy=%02X/%02X PC=%04X IFF=%02X\n",
                    crtc_regs[1], crtc_regs[6], crtc_regs[9], (crtc_regs[12] << 8) | crtc_regs[13], pal_b, pal_r, pal_g,
                    priority_reg, (int)hres_mode, x1PpiRead(0x1A02), x1JoyPort(0), x1JoyPort(1), cpu.PC.W, cpu.IFF);
    }
#endif
  } else {
    vTaskDelay(1);
  }
}
