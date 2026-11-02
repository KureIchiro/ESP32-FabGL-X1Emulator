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

static int keyCode(int k) {
    if (k < 0) return 0;
    const uint8_t mod = x1_key_matrix[14];
    const bool shift = !(mod & 0x02), ctrl = !(mod & 0x01), caps = !(mod & 0x08), graph = !(mod & 0x10);
    if (k >= X1K_TEN0 && k <= X1K_TEN0 + 15) {
        static const uint8_t tenCode[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
                                             '-', '+', '*', '/', '.', '=' };
        return 0x100 | tenCode[k - X1K_TEN0];
    }
    if (k == X1K_TENCOMMA) return 0x100 | ',';
    if (k >= X1K_F1 && k <= X1K_F1 + 4) return 0x100 | (0x71 + (k - X1K_F1) + (shift ? 5 : 0));
    if (k >= 0x60) return 0;
    int c = k;
    if (shift) {
        static const char sym[16] = { ' ', '!', '"', '#', '$', '%', '&', '\'', '(', ')', '*', '+', '<', '=', '>', '?' };
        if (c >= 0x30 && c <= 0x3F) c = (uint8_t)sym[c - 0x30];
        else if (c == 0x2C) c = '<';
        else if (c == 0x2D) c = '=';
        else if (c == 0x2E) c = '>';
        else if (c == 0x2F) c = '?';
        else if (c == 0x40) c = 0x60;
        else if (c == 0x5B) c = 0x7B;
        else if (c == 0x5D) c = 0x7D;
        else if (c == 0x5F) c = 0x7F;
        else if (c == 0x08) c = 0x12;
        else if (c == 0x0B) c = 0x0C;
    } else {
        if (ctrl && c >= 0x40 && c <= 0x5F) c -= 0x40;
        else if (c >= 0x41 && c <= 0x5A) c += 0x20;
    }
    if (caps && ((c >= 0x41 && c <= 0x5A) || (c >= 0x61 && c <= 0x7A))) c ^= 0x20;
    if (graph) c |= 0x80;
    return c;
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
    if (line >= x1_lines_frame) line = 0;
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
        int cyc = x1_cyc_line;
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

bool x1SubIrqPending() {
    return x1SioIrqPending() || keyIrqReady() || ctcIrq(ctcMain) || ctcIrq(ctcFm);
}

uint8_t x1SubIrqAck() {
    if (x1SioIrqPending()) return x1SioIrqAck();
    if (keyIrqReady()) {
        sub_cmd = 0xE6;
        response[0] = keyQ[keyQTail][0];
        response[1] = keyQ[keyQTail][1];
        keyQTail = (keyQTail + 1) & 15;
        resp_ptr = 0;
        resp_len = 2;
        obf = true;
        return key_irq_vector;
    }
    Ctc *const chain[2] = { &ctcMain, &ctcFm };
    for (Ctc *t : chain) {
        for (int ch = 0; ch < 4; ch++) {
            if (t->ch[ch].irq) {
                t->ch[ch].irq = false;
                return (uint8_t)(t->vector | (ch << 1));
            }
        }
    }
    return 0xFF;
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
