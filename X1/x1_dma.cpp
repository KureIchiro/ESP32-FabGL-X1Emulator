// ============================================================================
//  Z80 DMA(1F80h〜1F8Fh。X1turbo の本体の DMA。X1turbo 版と同じファイル)。Common Source Project の z80dma.cpp と同じ動き。
//  FDC の DRQ が RDY につながっている(反転。RDY の端子 = DRQ でない)。ディスクの読み書きのほか、
//  メモリ → I/O(VRAM)の転送にも使われる。
//  転送はまとめて行う(CSP の SINGLE_MODE_DMA でない動き): 有効にしたとき・Continue・Force Ready・
//  RDY が来たとき(1 ラインごとに見る)に、ブロックの終わりか RDY がなくなるまで転送する。
//  バスを使ったサイクルは数えておき、CPU の実行から引く(x1DmaTakeCycles)。
// ============================================================================
#pragma GCC optimize("O3")
#include "x1.h"
#include <string.h>

enum {
    R_WR0, R_WR1, R_WR2, R_WR3, R_WR4, R_WR5, R_WR6,
    R_PORTA_ADDR_L, R_PORTA_ADDR_H, R_BLOCKLEN_L, R_BLOCKLEN_H,
    R_PORTA_TIMING, R_PORTB_TIMING, R_MASK, R_MATCH,
    R_PORTB_ADDR_L, R_PORTB_ADDR_H, R_INT_CTRL, R_PULSE, R_VECTOR, R_READ_MASK,
    R_NUM
};
static uint8_t reg[R_NUM];
static int wrQueue[8], wrNum, wrPtr;
static uint8_t rdBuf[8];
static int rdNum, rdPtr;
static uint16_t addrA, addrB;
static int upcount;
static uint8_t status;
static bool enabled, forceReady, dmaStop, reqIntr;
static uint8_t vector;
static int stolen;

static inline uint16_t portAAddr() { return (uint16_t)(reg[R_PORTA_ADDR_L] | (reg[R_PORTA_ADDR_H] << 8)); }
static inline uint16_t portBAddr() { return (uint16_t)(reg[R_PORTB_ADDR_L] | (reg[R_PORTB_ADDR_H] << 8)); }
static inline uint16_t blockLen() { return (uint16_t)(reg[R_BLOCKLEN_L] | (reg[R_BLOCKLEN_H] << 8)); }
static inline bool portAIsSource() { return (reg[R_WR0] & 0x04) != 0; }
static inline bool portAIsIo() { return (reg[R_WR1] & 0x08) != 0; }
static inline bool portBIsIo() { return (reg[R_WR2] & 0x08) != 0; }
static inline int  transferMode() { return reg[R_WR0] & 3; }
static inline int  operatingMode() { return (reg[R_WR4] >> 5) & 3; }
static inline int  cycleLen(uint8_t t, bool io) { return (t & 3) != 3 ? 4 - (t & 3) : (io ? 4 : 3); }

static bool nowReady() {
    if (forceReady) return true;
    bool drq = x1FdcDrq();
    return (reg[R_WR5] & 0x08) ? !drq : drq;
}

void x1DmaReset() {
    memset(reg, 0, sizeof(reg));
    reg[R_PORTA_TIMING] = reg[R_PORTB_TIMING] = 3;
    wrNum = wrPtr = rdNum = rdPtr = 0;
    addrA = addrB = 0;
    upcount = 0;
    status = 0x30;
    enabled = forceReady = dmaStop = reqIntr = false;
    vector = 0;
    stolen = 0;
}

static void updateReadBuffer() {
    rdNum = rdPtr = 0;
    uint8_t m = reg[R_READ_MASK];
    if (m & 0x01) rdBuf[rdNum++] = (uint8_t)(status | (nowReady() ? 0 : 2) | (reqIntr ? 0 : 8));
    if (m & 0x02) rdBuf[rdNum++] = (uint8_t)upcount;
    if (m & 0x04) rdBuf[rdNum++] = (uint8_t)(upcount >> 8);
    if (m & 0x08) rdBuf[rdNum++] = (uint8_t)addrA;
    if (m & 0x10) rdBuf[rdNum++] = (uint8_t)(addrA >> 8);
    if (m & 0x20) rdBuf[rdNum++] = (uint8_t)addrB;
    if (m & 0x40) rdBuf[rdNum++] = (uint8_t)(addrB >> 8);
}

static void requestIntr(int level) {
    if (!(reg[R_WR3] & 0x20)) return;
    reqIntr = true;
    vector = (reg[R_INT_CTRL] & 0x20) ? (uint8_t)((reg[R_VECTOR] & 0xF9) | (level << 1)) : reg[R_VECTOR];
}

static uint8_t rd(uint16_t a, bool io) { return io ? x1ReadIO(a) : x1ReadByte(a); }
static void wr(uint16_t a, bool io, uint8_t v) { if (io) x1WriteIO(a, v); else x1WriteByte(a, v); }
static inline uint16_t step(uint16_t a, uint8_t wr12) { return (wr12 & 0x20) ? a : (wr12 & 0x10) ? (uint16_t)(a + 1) : (uint16_t)(a - 1); }

static void doDma() {
    if (!enabled) return;
    const uint16_t bl = blockLen();
    const int blocklen = bl == 0 ? 65537 : bl == 0xFFFF ? 65536 : bl + 1;
    bool found = false, occurred = false;
    const int lenA = cycleLen(reg[R_PORTA_TIMING], portAIsIo()), lenB = cycleLen(reg[R_PORTB_TIMING], portBIsIo());
    int guard = 0;
restart:
    while (enabled && nowReady() && !(upcount >= blocklen || found)) {
        if (++guard > 70000) break;
        if (dmaStop) {
            if (upcount < blocklen) upcount++;
            dmaStop = false;
        } else {
            const bool aSrc = portAIsSource();
            uint8_t data = aSrc ? rd(addrA, portAIsIo()) : rd(addrB, portBIsIo());
            stolen += aSrc ? lenA : lenB;
            int tm = transferMode();
            if (tm == 1 || tm == 3) {
                if (aSrc) wr(addrB, portBIsIo(), data); else wr(addrA, portAIsIo(), data);
                stolen += aSrc ? lenB : lenA;
            }
            if ((tm == 2 || tm == 3) && ((data & reg[R_MASK]) == (reg[R_MATCH] & reg[R_MASK]))) found = true;
            upcount++;
            occurred = true;
            if (found || (bl == 0 && !nowReady())) {
                if (upcount < blocklen) upcount--;
                dmaStop = true;
                break;
            }
        }
        if (portAIsSource()) {
            addrA = step(addrA, reg[R_WR1]);
            if (transferMode() != 2) addrB = step(addrB, reg[R_WR2]);
        } else {
            addrB = step(addrB, reg[R_WR2]);
            if (transferMode() != 2) addrA = step(addrA, reg[R_WR1]);
        }
    }
    if (occurred && (upcount == blocklen || found)) {
        if ((reg[R_WR5] & 0x20) && upcount == blocklen && !forceReady) {
            upcount = 0;
            goto restart;
        }
        status = 1;
        if (!found) status |= 0x10;
        if (upcount != blocklen) status |= 0x20;
        enabled = false;
        int level = 0;
        if (upcount == blocklen && (reg[R_INT_CTRL] & 0x02)) level |= 2;
        if (found && (reg[R_INT_CTRL] & 0x01)) level |= 1;
        if (level) requestIntr(level);
    }
}

void x1DmaWrite(uint8_t data) {
    if (wrNum == 0) {
        if ((data & 0x87) == 0x00) {
            reg[R_WR2] = data;
            if (data & 0x40) wrQueue[wrNum++] = R_PORTB_TIMING;
        } else if ((data & 0x87) == 0x04) {
            reg[R_WR1] = data;
            if (data & 0x40) wrQueue[wrNum++] = R_PORTA_TIMING;
        } else if ((data & 0x80) == 0x00) {
            reg[R_WR0] = data;
            if (data & 0x08) wrQueue[wrNum++] = R_PORTA_ADDR_L;
            if (data & 0x10) wrQueue[wrNum++] = R_PORTA_ADDR_H;
            if (data & 0x20) wrQueue[wrNum++] = R_BLOCKLEN_L;
            if (data & 0x40) wrQueue[wrNum++] = R_BLOCKLEN_H;
        } else if ((data & 0x83) == 0x80) {
            reg[R_WR3] = data;
            if (data & 0x08) wrQueue[wrNum++] = R_MASK;
            if (data & 0x10) wrQueue[wrNum++] = R_MATCH;
            enabled = (data & 0x40) != 0;
        } else if ((data & 0x83) == 0x81) {
            reg[R_WR4] = data;
            if (data & 0x04) wrQueue[wrNum++] = R_PORTB_ADDR_L;
            if (data & 0x08) wrQueue[wrNum++] = R_PORTB_ADDR_H;
            if (data & 0x10) wrQueue[wrNum++] = R_INT_CTRL;
        } else if ((data & 0xC7) == 0x82) {
            reg[R_WR5] = data;
        } else if ((data & 0x83) == 0x83) {
            reg[R_WR6] = data;
            enabled = false;
            if (data != 0xD3 && data != 0xBF && data != 0xA7 && data != 0x87 && data != 0x83 && data != 0xBB) dmaStop = false;
            switch (data) {
                case 0xC3:
                    forceReady = false; reqIntr = false; status = 0x30;
                    reg[R_PORTA_TIMING] |= 3; reg[R_PORTB_TIMING] |= 3;
                    reg[R_WR3] &= ~0x20; upcount = 0; reg[R_WR5] &= ~0x30;
                    break;
                case 0xC7: reg[R_PORTA_TIMING] |= 3; break;
                case 0xCB: reg[R_PORTB_TIMING] |= 3; break;
                case 0xCF:
                    forceReady = false; addrA = portAAddr(); addrB = portBAddr(); upcount = 0; status |= 0x30;
                    break;
                case 0xD3:
                    upcount = (dmaStop && upcount != (blockLen() + 1)) ? -1 : 0;
                    enabled = true; status |= 0x30;
                    doDma();
                    break;
                case 0xAF: reg[R_WR3] &= ~0x20; break;
                case 0xAB: reg[R_WR3] |= 0x20; break;
                case 0xA3: reg[R_WR3] &= ~0x20; reqIntr = false; forceReady = false; break;
                case 0xB7: break;
                case 0xBF: reg[R_READ_MASK] = 1; updateReadBuffer(); break;
                case 0x8B: status |= 0x30; reqIntr = false; break;
                case 0xA7: updateReadBuffer(); break;
                case 0xB3: forceReady = true; doDma(); break;
                case 0x87: enabled = true; doDma(); break;
                case 0x83: enabled = false; break;
                case 0xBB: wrQueue[wrNum++] = R_READ_MASK; break;
                default: break;
            }
        }
        wrPtr = 0;
    } else {
        int r = wrQueue[wrPtr++];
        reg[r] = data;
        if (r == R_INT_CTRL) {
            wrNum = wrPtr = 0;
            if (data & 0x08) wrQueue[wrNum++] = R_PULSE;
            if (data & 0x10) wrQueue[wrNum++] = R_VECTOR;
            return;
        }
        if (r == R_READ_MASK) updateReadBuffer();
        if (wrPtr >= wrNum) wrNum = wrPtr = 0;
    }
}

uint8_t x1DmaRead() {
    if (rdNum == 0) return (uint8_t)(status | (nowReady() ? 0 : 2) | (reqIntr ? 0 : 8));
    uint8_t v = rdBuf[rdPtr];
    if (++rdPtr >= rdNum) rdPtr = 0;
    return v;
}

void x1DmaRdy(bool) {
    if (enabled && nowReady()) doDma();
}

bool x1DmaIrqPending() { return reqIntr; }
uint8_t x1DmaIrqAck() { reqIntr = false; enabled = false; return vector; }

int x1DmaTakeCycles() {
    int s = stolen;
    stolen = 0;
    return s;
}
