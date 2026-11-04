// ============================================================================
//  X1 サブCPU通信プロトコル / i8255 PPI / Z80 CTC(簡易版)
//  方針B(プロトコル近似)の実装。
//  実機はキーボードMCU→サブCPU(共にMCS-48)→8255ハンドシェイク→Z80という
//  3段構成だが、ファームウェアは実行せず観測可能な入出力結果をネイティブ
//  コードで再現する。カセットはユーザー指示によりスコープ外（コマンドの
//  受理のみ行い、実際のテープ制御は行わない）。
// ============================================================================
// 【最適化】Arduinoのesp32コアは既定で -Os。スキャンライン駆動 はフレームごとに
// 数万回まわるため、この翻訳単位だけ O3 に引き上げる。
// (X1.ino:20 の #pragma は .ino の翻訳単位にしか効かない)
#pragma GCC optimize("O3")

#include "x1.h"
#include <string.h>

volatile uint8_t x1_key_matrix[X1_KEY_ROWS] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static uint8_t sub_cmd;
static uint8_t response[8];
static int     resp_len;
static int     resp_ptr;
static bool    obf;

struct X1Rtc {
    uint8_t sec, min, hour, day, wday, month, year;
};
static X1Rtc rtc = { 0, 0, 0, 1, 0, 1, 0 };

static void rtcTick1s() {
    static const uint8_t dpm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    rtc.sec++;
    if (rtc.sec >= 60) { rtc.sec = 0; rtc.min++; }
    if (rtc.min >= 60) { rtc.min = 0; rtc.hour++; }
    if (rtc.hour >= 24) { rtc.hour = 0; rtc.day++; rtc.wday = (rtc.wday + 1) % 7; }
    uint8_t days_this_month = dpm[(rtc.month - 1) % 12];
    if (rtc.month == 2 && (rtc.year % 4) == 0) days_this_month++;
    if (rtc.day > days_this_month) { rtc.day = 1; rtc.month++; }
    if (rtc.month > 12) { rtc.month = 1; rtc.year = (rtc.year + 1) % 100; }
}
static inline uint8_t toBCD(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static uint8_t key_irq_vector;
static uint8_t keyQ[16][2];
static int     keyQHead, keyQTail;

static inline bool keyDown(int k) { return !(x1_key_matrix[k >> 3] & (1 << (k & 7))); }

static int lastKey = -1;
static int keyRepCnt;
#define KEY_REP_FIRST 125
#define KEY_REP_NEXT  12

struct X1KeyCode { uint8_t key, code[6]; };
static const X1KeyCode keyCodeTable[] = {
    { 0x03, { 0x13, 0x03, 0x03, 0x13, 0x13, 0x03 } },
    { 0x08, { 0x08, 0x12, 0x08, 0x00, 0x08, 0x12 } },
    { 0x09, { 0x09, 0x09, 0x09, 0x00, 0x09, 0x09 } },
    { 0x0B, { 0x0B, 0x0C, 0x0B, 0x0B, 0x0B, 0x0C } },
    { 0x0D, { 0x0D, 0x0D, 0x0D, 0x00, 0x0D, 0x0D } },
    { 0x12, { 0x12, 0x12, 0x12, 0x12, 0x12, 0x12 } },
    { 0x1B, { 0x1B, 0x1B, 0x1B, 0x00, 0x1B, 0x1B } },
    { 0x1C, { 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C } },
    { 0x1D, { 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D } },
    { 0x1E, { 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E } },
    { 0x1F, { 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F } },
    { 0x20, { 0x20, 0x20, 0x00, 0x00, 0x20, 0x20 } },
    { 0x2C, { 0x2C, 0x3C, 0x00, 0x87, 0xC8, 0xA4 } },
    { 0x2D, { 0x2D, 0x3D, 0x2D, 0x8C, 0xCE, 0xCE } },
    { 0x2E, { 0x2E, 0x3E, 0x00, 0x88, 0xD9, 0xA1 } },
    { 0x2F, { 0x2F, 0x3F, 0x00, 0xFE, 0xD2, 0xA5 } },
    { 0x30, { 0x30, 0x30, 0x30, 0xFA, 0xDC, 0xA6 } },
    { 0x31, { 0x31, 0x21, 0x31, 0xF1, 0xC7, 0xC7 } },
    { 0x32, { 0x32, 0x22, 0x32, 0xF2, 0xCC, 0xCC } },
    { 0x33, { 0x33, 0x23, 0x33, 0xF3, 0xB1, 0xA7 } },
    { 0x34, { 0x34, 0x24, 0x34, 0xF4, 0xB3, 0xA9 } },
    { 0x35, { 0x35, 0x25, 0x35, 0xF5, 0xB4, 0xAA } },
    { 0x36, { 0x36, 0x26, 0x36, 0xF6, 0xB5, 0xAB } },
    { 0x37, { 0x37, 0x27, 0x37, 0xF7, 0xD4, 0xAC } },
    { 0x38, { 0x38, 0x28, 0x38, 0xF8, 0xD5, 0xAD } },
    { 0x39, { 0x39, 0x29, 0x39, 0xF9, 0xD6, 0xAE } },
    { 0x3A, { 0x3A, 0x2A, 0x3A, 0xFD, 0xB9, 0xB9 } },
    { 0x3B, { 0x3B, 0x2B, 0x3B, 0x89, 0xDA, 0xDA } },
    { 0x40, { 0x40, 0x7E, 0x40, 0x8A, 0xDE, 0xDE } },
    { 0x41, { 0x61, 0x41, 0x01, 0x7F, 0xC1, 0xC1 } },
    { 0x42, { 0x62, 0x42, 0x02, 0x84, 0xBA, 0xBA } },
    { 0x43, { 0x63, 0x43, 0x03, 0x82, 0xBF, 0xBF } },
    { 0x44, { 0x64, 0x44, 0x04, 0xEA, 0xBC, 0xBC } },
    { 0x45, { 0x65, 0x45, 0x05, 0xE2, 0xB2, 0xA8 } },
    { 0x46, { 0x66, 0x46, 0x06, 0xEB, 0xCA, 0xCA } },
    { 0x47, { 0x67, 0x47, 0x07, 0xEC, 0xB7, 0xB7 } },
    { 0x48, { 0x68, 0x48, 0x08, 0xED, 0xB8, 0xB8 } },
    { 0x49, { 0x69, 0x49, 0x09, 0xE7, 0xC6, 0xC6 } },
    { 0x4A, { 0x6A, 0x4A, 0x0A, 0xEE, 0xCF, 0xCF } },
    { 0x4B, { 0x6B, 0x4B, 0x0B, 0xEF, 0xC9, 0xC9 } },
    { 0x4C, { 0x6C, 0x4C, 0x0C, 0x8E, 0xD8, 0xD8 } },
    { 0x4D, { 0x6D, 0x4D, 0x0D, 0x86, 0xD3, 0xD3 } },
    { 0x4E, { 0x6E, 0x4E, 0x0E, 0x85, 0xD0, 0xD0 } },
    { 0x4F, { 0x6F, 0x4F, 0x0F, 0xF0, 0xD7, 0xD7 } },
    { 0x50, { 0x70, 0x50, 0x10, 0x8D, 0xBE, 0xBE } },
    { 0x51, { 0x71, 0x51, 0x11, 0xE0, 0xC0, 0xC0 } },
    { 0x52, { 0x72, 0x52, 0x12, 0xE3, 0xBD, 0xBD } },
    { 0x53, { 0x73, 0x53, 0x13, 0xE9, 0xC4, 0xC4 } },
    { 0x54, { 0x74, 0x54, 0x14, 0xE4, 0xB6, 0xB6 } },
    { 0x55, { 0x75, 0x55, 0x15, 0xE6, 0xC5, 0xC5 } },
    { 0x56, { 0x76, 0x56, 0x16, 0x83, 0xCB, 0xCB } },
    { 0x57, { 0x77, 0x57, 0x17, 0xE1, 0xC3, 0xC3 } },
    { 0x58, { 0x78, 0x58, 0x18, 0x81, 0xBB, 0xBB } },
    { 0x59, { 0x79, 0x59, 0x19, 0xE5, 0xDD, 0xDD } },
    { 0x5A, { 0x7A, 0x5A, 0x1A, 0x80, 0xC2, 0xAF } },
    { 0x5B, { 0x5B, 0x7B, 0x1B, 0xFC, 0xDF, 0xA2 } },
    { 0x5C, { 0x5C, 0x7C, 0x1C, 0xFB, 0xB0, 0xB0 } },
    { 0x5D, { 0x5D, 0x7D, 0x1D, 0xE8, 0xD1, 0xA3 } },
    { 0x5E, { 0x5E, 0x60, 0x1E, 0x8B, 0xCD, 0xCD } },
    { 0x5F, { 0x00, 0x5F, 0x1F, 0xFF, 0xDB, 0xDB } },
    { 0x60, { 0x30, 0x30, 0x30, 0x8F, 0x30, 0x30 } },
    { 0x61, { 0x31, 0x31, 0x31, 0x99, 0x31, 0x31 } },
    { 0x62, { 0x32, 0x32, 0x32, 0x92, 0x32, 0x32 } },
    { 0x63, { 0x33, 0x33, 0x33, 0x98, 0x33, 0x33 } },
    { 0x64, { 0x34, 0x34, 0x34, 0x95, 0x34, 0x34 } },
    { 0x65, { 0x35, 0x35, 0x35, 0x96, 0x35, 0x35 } },
    { 0x66, { 0x36, 0x36, 0x36, 0x94, 0x36, 0x36 } },
    { 0x67, { 0x37, 0x37, 0x37, 0x9A, 0x37, 0x37 } },
    { 0x68, { 0x38, 0x38, 0x38, 0x93, 0x38, 0x38 } },
    { 0x69, { 0x39, 0x39, 0x39, 0x97, 0x39, 0x39 } },
    { 0x6A, { 0x2D, 0x2D, 0x2D, 0x9C, 0x2D, 0x2D } },
    { 0x6B, { 0x2B, 0x2B, 0x2B, 0x9D, 0x2B, 0x2B } },
    { 0x6C, { 0x2A, 0x2A, 0x2A, 0x9B, 0x2A, 0x2A } },
    { 0x6D, { 0x2F, 0x3F, 0x2F, 0x9E, 0xD2, 0xA5 } },
    { 0x6E, { 0x2E, 0x2E, 0x2E, 0x91, 0x2E, 0x2E } },
    { 0x6F, { 0x3D, 0x3D, 0x3D, 0x90, 0x3D, 0x3D } },
    { 0x78, { 0x71, 0x76, 0x00, 0x00, 0x71, 0x76 } },
    { 0x79, { 0x72, 0x77, 0x00, 0x00, 0x72, 0x77 } },
    { 0x7A, { 0x73, 0x78, 0x00, 0x00, 0x73, 0x78 } },
    { 0x7B, { 0x74, 0x79, 0x00, 0x00, 0x74, 0x79 } },
    { 0x7C, { 0x75, 0x7A, 0x00, 0x00, 0x75, 0x7A } },
    { 0x7D, { 0x2C, 0x3C, 0x00, 0x87, 0xC8, 0xA4 } },
};

static int keyCode(int k) {
    if (k < 0) return 0;
    const uint8_t mod = x1_key_matrix[14];
    const bool shift = !(mod & 0x02), ctrl = !(mod & 0x01), kana = !(mod & 0x04), caps = !(mod & 0x08), graph = !(mod & 0x10);
    for (const X1KeyCode &e : keyCodeTable) {
        if (e.key != k) continue;
        int c = kana ? e.code[shift ? 5 : 4] : ctrl ? e.code[2] : graph ? e.code[3] : e.code[shift ? 1 : 0];
        if (!c) return 0;
        if (caps && !kana && !ctrl && !graph && k >= 0x41 && k <= 0x5A) c ^= 0x20;
        const bool ten = (k >= X1K_TEN0 && k <= X1K_TEN0 + 15) || k == X1K_TENCOMMA || (k >= X1K_F1 && k <= X1K_F1 + 4);
        return ten ? (0x100 | c) : c;
    }
    return 0;
}

static uint8_t keyShift(int code, bool rep) {
    uint8_t val = 0xE0 | (x1_key_matrix[14] & 0x1F);
    if (code) val &= ~0x40;
    if (code & 0x100) val &= ~0x80;
    if (rep) val &= ~0x20;
    return val;
}

static uint8_t gameKey(int n) {
    static const uint8_t keys[3][8] = {
        { 'C', 'X', 'Z', 'D', 'A', 'E', 'W', 'Q' },
        { X1K_TEN0 + 3, X1K_TEN0 + 6, X1K_TEN0 + 9, X1K_TEN0 + 2, X1K_TEN0 + 8, X1K_TEN0 + 1, X1K_TEN0 + 4, X1K_TEN0 + 7 },
        { 0x0D, ' ', 0x09, X1K_TEN0 + 12, X1K_TEN0 + 11, X1K_TEN0 + 10, '1', 0x1B },
    };
    uint8_t v = 0;
    for (int b = 0; b < 8; b++) if (keyDown(keys[n][b])) v |= (uint8_t)(1 << b);
    if (n == 1) {
        if (keyDown(0x1E)) v |= 0x10;
        if (keyDown(0x1F)) v |= 0x08;
        if (keyDown(0x1D)) v |= 0x40;
        if (keyDown(0x1C)) v |= 0x02;
    }
    return v;
}

static uint8_t cmt_current_cmd;

uint8_t x1SubIoRead() {
    if (resp_len <= 0) return 0xFF;
    uint8_t v = response[resp_ptr];
    resp_ptr++;
    resp_len--;
    if (resp_len <= 0) obf = false;
    return v;
}

static uint8_t tv_ctrl;
static int     data_left;
static int     data_idx;
static uint8_t data_buf[3];

static void subCommandData(uint8_t data) {
    data_buf[data_idx++] = data;
    if (--data_left > 0) return;
    switch (sub_cmd) {
        case 0xE4: key_irq_vector = data_buf[0]; break;
        case 0xE7: tv_ctrl = data_buf[0]; break;
        case 0xE9: cmt_current_cmd = data_buf[0]; break;
        case 0xEC:
            rtc.day   = (data_buf[0] >> 4) * 10 + (data_buf[0] & 0x0F);
            rtc.month = data_buf[1] >> 4;
            rtc.wday  = data_buf[1] & 0x0F;
            rtc.year  = (data_buf[2] >> 4) * 10 + (data_buf[2] & 0x0F);
            if (rtc.month < 1 || rtc.month > 12) rtc.month = 1;
            if (rtc.day < 1) rtc.day = 1;
            break;
        case 0xEE:
            rtc.hour = (data_buf[0] >> 4) * 10 + (data_buf[0] & 0x0F);
            rtc.min  = (data_buf[1] >> 4) * 10 + (data_buf[1] & 0x0F);
            rtc.sec  = (data_buf[2] >> 4) * 10 + (data_buf[2] & 0x0F);
            break;
        default: break;
    }
}

void x1SubIoWrite(uint8_t data) {
    if (data_left > 0) {
        subCommandData(data);
        return;
    }

    sub_cmd = data;
    resp_ptr = 0;
    resp_len = 0;
    data_idx = 0;

    switch (data) {
        case 0xE3:
            response[0] = gameKey(0);
            response[1] = gameKey(1);
            response[2] = gameKey(2);
            resp_len = 3;
            break;
        case 0xE4:
            data_left = 1;
            break;
        case 0xE6: {
            int code = keyCode(lastKey);
            response[0] = keyShift(code, false);
            response[1] = (uint8_t)code;
            resp_len = 2;
            break;
        }
        case 0xE7:
            data_left = 1;
            break;
        case 0xE8:
            response[0] = tv_ctrl;
            resp_len = 1;
            break;
        case 0xE9:
            data_left = 1;
            break;
        case 0xEA:
            response[0] = cmt_current_cmd;
            resp_len = 1;
            break;
        case 0xEB:
            response[0] = 0x00;
            resp_len = 1;
            break;
        case 0xEC:
        case 0xEE:
            data_left = 3;
            break;
        case 0xED:
            response[0] = toBCD(rtc.day);
            response[1] = (uint8_t)((rtc.month << 4) | (rtc.wday & 0xF));
            response[2] = toBCD(rtc.year);
            resp_len = 3;
            break;
        case 0xEF:
            response[0] = toBCD(rtc.hour);
            response[1] = toBCD(rtc.min);
            response[2] = toBCD(rtc.sec);
            resp_len = 3;
            break;
        default:
            break;
    }
    obf = (resp_len > 0);
}

static uint8_t ppi_porta, ppi_portc_out;

bool x1RomSelected();

static uint8_t ppiPortB() {
    int line = x1_current_scanline + 1;
    if (line >= X1_LINES_PER_FRAME) line = 0;
    const int rh   = (crtc_regs[9] & 0x1F) + 1;
    const int vblank_line = crtc_regs[6] * rh;
    const int vsync_line  = crtc_regs[7] * rh;
    uint8_t v = 0x01;
    if (line < vblank_line || (vblank_line == 0 && line == 0))  v |= 0x80;
    if (!obf)                v |= 0x20;
    if (!x1RomSelected())    v |= 0x10;
    if (line >= vsync_line)  v |= 0x04;
    return v;
}

uint8_t x1PpiRead(uint16_t port) {
    switch (port & 0x03) {
        case 0: return ppi_porta;
        case 1: return ppiPortB();
        case 2: return ppi_portc_out;
        default: return 0xFF;
    }
}

void x1PpiWrite(uint16_t port, uint8_t val) {
    switch (port & 0x03) {
        case 0: ppi_porta = val; break;
        case 2:
            ppi_portc_out = val;
            x1SetPortCBits(val & 0x20, val & 0x40);
            break;
        case 3:
            if (!(val & 0x80)) {
                uint8_t bit = (uint8_t)(1 << ((val >> 1) & 7));
                ppi_portc_out = (val & 1) ? (ppi_portc_out | bit) : (ppi_portc_out & ~bit);
            } else {
                ppi_porta = 0;
                ppi_portc_out = 0;
            }
            x1SetPortCBits(ppi_portc_out & 0x20, ppi_portc_out & 0x40);
            break;
        default: break;
    }
}

struct CtcCh {
    uint8_t ctrl;
    uint8_t tconst;
    int     count;
    int     presc;
    bool    wait_tc;
    bool    running;
    bool    irq;
};
struct Ctc {
    CtcCh   ch[4];
    uint8_t vector;
};
static Ctc ctcMain;
static Ctc ctcFm;
static inline Ctc &ctcOf(uint16_t port) { return ((port & 0xFF00) == 0x0700) ? ctcFm : ctcMain; }

static inline int ctcTc(const CtcCh &c) { return c.tconst ? c.tconst : 256; }

uint8_t x1CtcRead(uint16_t port) {
    int ch = port & 0x03;
    return (uint8_t)ctcOf(port).ch[ch].count;
}

void x1CtcWrite(uint16_t port, uint8_t val) {
    int ch = port & 0x03;
    Ctc &t = ctcOf(port);
    CtcCh &c = t.ch[ch];
    if (c.wait_tc) {
        c.tconst  = val;
        c.count   = ctcTc(c);
        c.presc   = (c.ctrl & 0x20) ? 256 : 16;
        c.wait_tc = false;
        c.running = true;
        return;
    }
    if (val & 0x01) {
        c.ctrl = val;
        if (val & 0x02) c.running = false;
        if (!(val & 0x80)) c.irq = false;
        if (val & 0x04) c.wait_tc = true;
        return;
    }
    if (ch == 0) t.vector = val & 0xF8;
}

static void ctcCountOne(Ctc &t, int ch);
static void ctcZeroCount(Ctc &t, int ch) {
    CtcCh &c = t.ch[ch];
    c.count = ctcTc(c);
    if (c.ctrl & 0x80) c.irq = true;
    if (ch == 0) {
        CtcCh &c3 = t.ch[3];
        if (c3.running && (c3.ctrl & 0x40)) ctcCountOne(t, 3);
    }
}
static void ctcCountOne(Ctc &t, int ch) {
    if (--t.ch[ch].count <= 0) ctcZeroCount(t, ch);
}

static void ctcTickLine(Ctc &t) {
    for (int ch = 0; ch < 4; ch++) {
        CtcCh &c = t.ch[ch];
        if (!c.running || (c.ctrl & 0x40)) continue;
        int cyc = X1_CYCLES_PER_LINE;
        while (cyc >= c.presc) {
            cyc -= c.presc;
            c.presc = (c.ctrl & 0x20) ? 256 : 16;
            ctcCountOne(t, ch);
        }
        c.presc -= cyc;
    }
}

void x1SubScanlineTick() {
    ctcTickLine(ctcMain);
    ctcTickLine(ctcFm);
}

static inline bool keyIrqReady() { return keyQHead != keyQTail && resp_len == 0 && data_left == 0; }
static inline bool ctcIrq(const Ctc &t) { return t.ch[0].irq || t.ch[1].irq || t.ch[2].irq || t.ch[3].irq; }

uint16_t x1SubIrqSources() {
    uint16_t m = 0;
    if (x1SioIrqPending()) m |= 0x0001;
    if (keyIrqReady()) m |= 0x0004;
    for (int ch = 0; ch < 4; ch++) {
        if (ctcMain.ch[ch].irq) m |= (uint16_t)(0x0008 << ch);
        if (ctcFm.ch[ch].irq) m |= (uint16_t)(0x0080 << ch);
    }
    return m;
}

uint8_t x1SubIrqAckSource(int src) {
    if (src == 0) return x1SioIrqAck();
    if (src == 2) {
        sub_cmd = 0xE6;
        response[0] = keyQ[keyQTail][0];
        response[1] = keyQ[keyQTail][1];
        keyQTail = (keyQTail + 1) & 15;
        resp_ptr = 0;
        resp_len = 2;
        obf = true;
        return key_irq_vector;
    }
    Ctc &t = (src < 7) ? ctcMain : ctcFm;
    const int ch = (src - 3) & 3;
    t.ch[ch].irq = false;
    return (uint8_t)(t.vector | (ch << 1));
}

void x1KeyPollTick() {
    static uint8_t prev[X1_KEY_ROWS];
    static bool first = true;
    uint8_t cur[X1_KEY_ROWS];
    memcpy(cur, (const void *)x1_key_matrix, X1_KEY_ROWS);
    if (first) {
        memcpy(prev, cur, X1_KEY_ROWS);
        first = false;
        return;
    }
    int pressed = -1;
    for (int r = 0; r < X1_KEY_ROWS; r++) {
        if (r == 14) continue;
        uint8_t nw = (uint8_t)(prev[r] & ~cur[r]);
        for (int b = 0; nw; b++, nw >>= 1)
            if ((nw & 1) && keyCode(r * 8 + b)) pressed = r * 8 + b;
    }
    bool send = false, rep = false;
    if (pressed >= 0) {
        lastKey = pressed;
        keyRepCnt = KEY_REP_FIRST;
        send = true;
    } else if (lastKey >= 0 && (cur[lastKey >> 3] & (1 << (lastKey & 7)))) {
        lastKey = -1;
        send = true;
    } else if (lastKey >= 0 && --keyRepCnt <= 0) {
        keyRepCnt = KEY_REP_NEXT;
        send = rep = true;
    }
#ifdef X1_TURBO_KEYBOARD
    if ((prev[14] ^ cur[14]) & 0x1F) send = true;
#endif
    memcpy(prev, cur, X1_KEY_ROWS);
    if (send && key_irq_vector != 0) {
        int code = keyCode(lastKey);
        uint8_t s = keyShift(code, rep), k = (uint8_t)code;
        int last = (keyQHead - 1) & 15;
        bool same = keyQHead != keyQTail && keyQ[last][0] == s && keyQ[last][1] == k;
        int next = (keyQHead + 1) & 15;
        if (!same && next != keyQTail) {
            keyQ[keyQHead][0] = s;
            keyQ[keyQHead][1] = k;
            keyQHead = next;
        }
    }
}

void x1RtcTick() {
    rtcTick1s();
}
