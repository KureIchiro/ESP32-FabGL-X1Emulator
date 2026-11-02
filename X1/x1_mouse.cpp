// ============================================================================
//  マウスボード(CZ-8BM2 相当): Z80 SIO(1F98h〜1F9Bh)とマウス
//  Common Source Project(vm/x1/x1.cpp・mouse.cpp)と同じつなぎ方:
//    ・SIO のチャンネル B にマウスがつながる。ソフトが WR5 の RTS を立てる(/RTS 端子が H→L)と、
//      マウスが 3 バイト(状態・X の移動量・Y の移動量)を送る(4800bps。1 バイト約 2.1ms)
//    ・状態: bit0 = 左ボタン、bit1 = 右ボタン、bit4/5 = X が +/- にあふれた、bit6/7 = Y が +/- にあふれた
//    ・ポート: 1F98h = A データ、1F99h = A 制御、1F9Ah = B データ、1F9Bh = B 制御
//    ・割込みはデイジーチェーンの先頭(CTC より上)
//  SIO は受信と割込みに要るところだけ(送信・同期モードなどは無し)。
// ============================================================================
#include "x1.h"
#include <string.h>

volatile int32_t x1_mouse_dx = 0, x1_mouse_dy = 0;
volatile uint8_t x1_mouse_buttons = 0;

static const int SIO_BYTE_LINES = 33;

struct SioCh {
    uint8_t ptr;
    uint8_t wr[8];
    uint8_t fifo[4];
    int     n;
    bool    firstArmed;
    uint8_t pend[3];
    int     pendN, pendPos, delay;
};
static SioCh sio[2];
static bool sioIrq;

static void chReset(SioCh &c) { memset(&c, 0, sizeof(c)); c.firstArmed = true; }

void x1SioReset() {
    chReset(sio[0]);
    chReset(sio[1]);
    sioIrq = false;
}

static void mouseSend() {
    int32_t dx = x1_mouse_dx, dy = x1_mouse_dy;
    x1_mouse_dx -= dx;
    x1_mouse_dy -= dy;
    uint8_t st = x1_mouse_buttons & 3;
    if (dx > 127)  { st |= 0x10; dx = 127; }  else if (dx < -128) { st |= 0x20; dx = -128; }
    if (dy > 127)  { st |= 0x40; dy = 127; }  else if (dy < -128) { st |= 0x80; dy = -128; }
    SioCh &c = sio[1];
    c.pend[0] = st; c.pend[1] = (uint8_t)dx; c.pend[2] = (uint8_t)dy;
    c.pendN = 3; c.pendPos = 0; c.delay = SIO_BYTE_LINES;
    c.n = 0;
}

static inline int rxIntMode(const SioCh &c) { return (c.wr[1] >> 3) & 3; }

void x1SioScanlineTick() {
    SioCh &c = sio[1];
    if (!c.pendN || --c.delay > 0) return;
    if ((c.wr[3] & 1) && c.n < 4) {
        c.fifo[c.n++] = c.pend[c.pendPos];
        int m = rxIntMode(c);
        if (m >= 2 || (m == 1 && c.firstArmed)) { sioIrq = true; c.firstArmed = false; }
    }
    c.pendPos++;
    if (--c.pendN) c.delay = SIO_BYTE_LINES;
}

uint8_t x1SioRead(uint16_t port) {
    SioCh &c = sio[(port >> 1) & 1];
    if (!(port & 1)) {
        if (!c.n) return 0xFF;
        uint8_t v = c.fifo[0];
        memmove(c.fifo, c.fifo + 1, --c.n);
        return v;
    }
    int r = c.ptr;
    c.ptr = 0;
    switch (r) {
        case 0: return (uint8_t)((c.n ? 0x01 : 0) | 0x04 | 0x08 | 0x20);
        case 1: return 0x01;
        case 2: return sio[1].wr[2];
        default: return 0x00;
    }
}

void x1SioWrite(uint16_t port, uint8_t v) {
    int ch = (port >> 1) & 1;
    SioCh &c = sio[ch];
    if (!(port & 1)) return;
    if (c.ptr == 0) {
        c.wr[0] = v;
        c.ptr = v & 7;
        switch ((v >> 3) & 7) {
            case 3: { uint8_t vec = sio[1].wr[2]; chReset(c); if (ch == 1) c.wr[2] = vec; break; }
            case 4: c.firstArmed = true; break;
            default: break;
        }
        return;
    }
    int r = c.ptr;
    c.ptr = 0;
    uint8_t old = c.wr[r];
    if (r == 2) { sio[1].wr[2] = v; return; }
    c.wr[r] = v;
    if (ch == 1 && r == 5 && (v & 0x02) && !(old & 0x02)) mouseSend();
}

bool x1SioIrqPending() { return sioIrq; }

uint8_t x1SioIrqAck() {
    sioIrq = false;
    uint8_t vec = sio[1].wr[2];
    if (sio[1].wr[1] & 0x04) vec = (uint8_t)((vec & 0xF1) | (2 << 1));
    return vec;
}
