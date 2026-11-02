// ============================================================================
//  X1turbo video renderer: テキスト/PCG/漢字 + ビットマップ + 優先度を 1 ラインずつ合成する
//  出力は 640x400 16色(1 画素 4bit、FabGL VGA16Controller のビューポート)。
//    色 0〜7  = テキストの固定 8 色(番号の bit0 = B、bit1 = R、bit2 = G)
//    色 8〜15 = ビットマップの色番号 0〜7 をパレット(1000h〜1200h)に通した色
//  パレットの書き換えは x1SetPaletteEntry で出力の色の表の 8〜15 を変えるだけなので、画面は描き直さない
//  (無印版はパレットが変わるたびにその色を使うラインを描き直していた)。
//  24kHz(1 フレーム 400 ライン以上)は 400 ライン、15kHz は 200 ラインを描き、偶数ラインの行バッファを
//  奇数ラインにも割り当てて 2 ラインに見せる(FabGL の割り込みは行ポインタの表を引いて出すので、表を書き換えるだけ)。
//
//  1 文字(8 ドット)の出力は 80 桁で 32bit 1 語、40 桁(横 2 倍)で 2 語。
//  VGA16 のバイトは「左の画素 = 上位 4bit、右の画素 = 下位 4bit」、語の中は下位バイトが左。
//  E80[v] / E40L[v] / E40H[v]: プレーンのバイト v(bit7 が左端)の点を、その画素の 4bit の bit0 に置いたもの。
//    ビットマップ = E[B] | E[R]<<1 | E[G]<<2 | 0x88888888(色 8〜15)、テキスト = E[b] | E[r]<<1 | E[g]<<2(色 0〜7)
//
//  turbo の文字(CSP display.cpp draw_text と同じ):
//    漢字 VRAM bit7 = 漢字(16 ライン、bit6 = 右半分)、属性 bit5 + 漢字 VRAM の bit7/bit4 = 16 ラインの外字(PCG 2 文字)、
//    1FD0h の bit0/bit2 = ANK 8x16、文字の中のラインは 1FD0h の bit0/bit2 で 1/2・そのまま・2 倍
//  ビットマップの番地(CSP draw_cg): 24kHz で 1FD0h bit1 = 0 なら偶数ラインはページ 0・奇数はページ 1、
//    それ以外は bit3 のページ。行の中のラインは 24kHz なら半分。bit2 = 1 なら 1 ラインの段が 400h ごと(16 段)
// ============================================================================
// 【最適化】Arduinoのesp32コアは既定で -Os。描画ループ はフレームごとに
// 数万回まわるため、この翻訳単位だけ O3 に引き上げる。
#pragma GCC optimize("O3")

#include "x1.h"
#include <string.h>

uint32_t x1Adr2KnjX1t(uint16_t adr);

#ifdef _MSC_VER
#define X1_NOINLINE    __declspec(noinline)
#define X1_FORCEINLINE __forceinline
#else
#define X1_NOINLINE    __attribute__((noinline))
#define X1_FORCEINLINE inline __attribute__((always_inline))
#endif

static uint32_t E80[256], E40L[256], E40H[256];
static uint32_t W4[16];
static uint8_t  priNib[256];
static uint32_t colMask[8];
static uint32_t ankTxtMul[8];
static bool tablesReady = false;

uint8_t **x1_vp_rows = nullptr;
static uint8_t *rowBuf[X1_SCREEN_H];
static bool mapped = false, mappedHireso = false;

static inline int nibPos(int k) { return 8 * (k >> 1) + ((k & 1) ? 0 : 4); }

void x1VideoBuildTables() {
    for (int v = 0; v < 256; v++) {
        uint32_t e = 0, l = 0, h = 0;
        for (int k = 0; k < 8; k++) {
            if (!((v >> (7 - k)) & 1)) continue;
            e |= 1u << nibPos(k);
            uint32_t pair = (1u << 4) | 1u;
            if (k < 4) l |= pair << (8 * k); else h |= pair << (8 * (k - 4));
        }
        E80[v] = e; E40L[v] = l; E40H[v] = h;
    }
    for (int v = 0; v < 16; v++) {
        uint8_t w = 0;
        for (int k = 0; k < 4; k++) if ((v >> (3 - k)) & 1) w |= (uint8_t)(0xC0 >> (2 * k));
        W4[v] = w;
    }
    for (int c = 0; c < 8; c++)
        colMask[c] = ((c & 1) ? 0xFFu : 0) | ((c & 2) ? 0xFF00u : 0) | ((c & 4) ? 0xFF0000u : 0);
    tablesReady = true;
    x1VideoUpdatePriority();
    x1VideoPaletteChanged();
}

static bool fixedSet = false;
static int16_t shownPal[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
void x1VideoPaletteChanged() {
    if (!fixedSet) {
        for (int t = 0; t < 8; t++) x1SetPaletteEntry(t, (t >> 1) & 1, (t >> 2) & 1, t & 1);
        fixedSet = true;
    }
    for (int c = 0; c < 8; c++) {
        int r = (pal_r >> c) & 1, g = (pal_g >> c) & 1, b = (pal_b >> c) & 1;
        if ((c == 0 && (x1_mode2 & 0x10)) || (c == 1 && (x1_mode2 & 0x20))) r = g = b = 0;
        int16_t v = (int16_t)(r << 1 | g << 2 | b);
        if (v == shownPal[c]) continue;
        shownPal[c] = v;
        x1SetPaletteEntry(8 + c, (uint8_t)r, (uint8_t)g, (uint8_t)b);
    }
}
static void forcePaletteOut() {
    fixedSet = false;
    for (int c = 0; c < 8; c++) shownPal[c] = -1;
    x1VideoPaletteChanged();
}

static inline uint32_t mixWord(uint32_t h, uint32_t w) { h = (h ^ w) * 0x9E3779B1u; return h ^ (h >> 16); }
uint32_t x1_vram_gen = 0;
uint32_t x1_gv_gen[2][512];
uint32_t x1_tx_gen[128];
uint32_t x1_pcg_gen = 0;
uint32_t x1_lines_drawn = 0;
uint32_t x1_lines_txtsame = 0;
static uint32_t videoGen = 1;
static uint32_t lineCg[X1_SCREEN_H], lineTx[X1_SCREEN_H], lineTxt[X1_SCREEN_H];
static uint8_t  lineFlags[X1_SCREEN_H];

void x1VideoInvalidate() { videoGen++; }

static uint8_t textKey;
void x1VideoUpdatePriority() {
    for (int i = 0; i < 256; i++) {
        uint8_t m = 0;
        if ((i & 0x08) && (priority_reg & (1 << (i & 7)))) m |= 0x0F;
        if ((i & 0x80) && (priority_reg & (1 << ((i >> 4) & 7)))) m |= 0xF0;
        priNib[i] = m;
    }
    textKey = ((x1_mode2 & 0x08) && (x1_mode2 & 7)) ? (uint8_t)(8 | (x1_mode2 & 7)) : 0;
    for (int c = 0; c < 8; c++) {
        uint32_t m = ((c & 1) ? 0x1u : 0) | ((c & 2) ? 0x100u : 0) | ((c & 4) ? 0x10000u : 0) | (c ? 0x1000000u : 0);
        if (textKey && c == (textKey & 7)) m = 0x1000000u;
        ankTxtMul[c] = m;
    }
    videoGen++;
}

static inline uint32_t txSum(int start, int n) {
    uint32_t s = 0;
    int c = start >> 4, cnt = ((start & 15) + n + 15) >> 4;
    for (int i = 0; i < cnt; i++) s = s * 0x01000193u + x1_tx_gen[(c + i) & 127];
    return s;
}
static inline uint32_t gvSum(int page, int ofs, int start, int n) {
    const uint32_t *g = x1_gv_gen[page];
    uint32_t s = 0;
    if (!(ofs & 0x400)) {
        int base = ofs >> 5, c = start >> 5, cnt = ((start & 31) + n + 31) >> 5;
        for (int i = 0; i < cnt; i++) s = s * 0x01000193u + g[(base + ((c + i) & 63)) & 511];
        return s;
    }
    int last = -1;
    for (int cx = 0; cx < n; cx++) {
        int ch = ((ofs | ((start + cx) & 0x7FF)) >> 5) & 511;
        if (ch != last) { s = s * 0x01000193u + g[ch]; last = ch; }
    }
    return s;
}

void x1VideoSetOutput(uint8_t **vpRows) {
    x1_vp_rows = vpRows;
    for (int y = 0; y < X1_SCREEN_H; y++) rowBuf[y] = vpRows[y];
    mapped = false;
}
static void mapRows(bool hireso) {
    if (!x1_vp_rows) return;
    for (int y = 0; y < X1_SCREEN_H; y++) x1_vp_rows[y] = rowBuf[hireso ? y : (y & ~1)];
    mapped = true;
    mappedHireso = hireso;
    videoGen++;
}
void x1VideoUnmapRows() {
    if (!x1_vp_rows) return;
    if (mapped && !mappedHireso)
        for (int y = 0; y < X1_SCREEN_H; y += 2) memcpy(rowBuf[y + 1], rowBuf[y], X1_ROW_BYTES);
    for (int y = 0; y < X1_SCREEN_H; y++) x1_vp_rows[y] = rowBuf[y];
    mapped = false;
    videoGen++;
}
void x1VideoResume() {
    forcePaletteOut();
    videoGen++;
}
void x1VideoFrameStart() {
    if (!mapped || mappedHireso != x1_hireso) mapRows(x1_hireso);
}

static inline int glyphLine(int kind, int yl, bool dh, int dy, int l) {
    int base = dh ? (yl + dy) >> 1 : yl;
    int shift;
    if (kind == 2) shift = ((x1_mode1_eff & 5) == 5) ? 1 : (x1_mode1_eff & 5) ? 0 : -1;
    else shift = (kind == 1 && (x1_mode1_eff & 1)) ? 1 : 0;
    if (shift == 1) return base >> 1;
    if (shift == -1) return (base << 1) | (dh ? (l & 1) : 0);
    return base;
}

struct TextLine {
    const uint8_t *tv, *av, *kv, *pcgB, *pcgR, *pcgG, *cg, *kj;
    int rowBase, stride, row, th, ty, xChars;
    int lnAnk, lnPcg, ln16;
    uint32_t keyPl;
    bool key, rowDblH, blank8, blinkPhase;
    uint8_t attrOr;
};

static inline uint32_t applyTextKey(uint32_t t, uint32_t keyPl) {
    const uint32_t d = (t ^ keyPl) & 0xFFFFFFu;
    const uint8_t km = (uint8_t)(t >> 24) & (uint8_t)~(d | (d >> 8) | (d >> 16));
    return t & ~(km * 0x010101u);
}

static int IRAM_ATTR X1_NOINLINE textCellSlow(TextLine &L, int cx, uint32_t *txt) {
    const uint16_t ofs = (L.rowBase + cx) & 0x7FF;
    const uint8_t code = L.tv[ofs], attr = L.av[ofs], knj = L.kv[ofs];
    L.attrOr |= attr;
    const bool blink = (attr & 0x10) && L.blinkPhase;
    const bool rev8  = (attr & 0x08) != 0;
    const int  color = attr & 7;
    int kind;
    if (attr & 0x20) kind = (knj & 0x90) ? 2 : 1;
    else kind = ((knj & 0x80) || (x1_mode1_eff & 5)) ? 2 : 0;
    int line;
    if ((attr & 0x40) && L.rowDblH) {
        int dy = 0;
        if (L.row > 0) {
            uint16_t pofs = (L.rowBase - L.stride + cx) & 0x7FF;
            if (L.tv[pofs] == code && L.av[pofs] == attr) dy = L.th;
        }
        line = glyphLine(kind, L.ty, true, dy, L.ty);
    } else {
        line = kind == 0 ? L.lnAnk : kind == 1 ? L.lnPcg : L.ln16;
    }
    uint8_t pb, pr, pg, op;
    if (attr & 0x20) {
        uint8_t b0, r0, g0;
        if (kind == 2) {
            int l16 = line & 15;
            int idx = ((((code & 0xFE) | (l16 & 1)) * 8) + (l16 >> 1)) & 0x7FF;
            b0 = L.pcgB[idx]; r0 = L.pcgR[idx]; g0 = L.pcgG[idx];
        } else if (L.blank8) {
            b0 = r0 = g0 = 0;
        } else {
            int idx = (code * 8 + (line & 7)) & 0x7FF;
            b0 = L.pcgB[idx]; r0 = L.pcgR[idx]; g0 = L.pcgG[idx];
        }
        pb = (color & 1) ? b0 : 0; pr = (color & 2) ? r0 : 0; pg = (color & 4) ? g0 : 0;
    } else {
        uint8_t pat;
        if (kind == 2) {
            uint32_t ko;
            if (knj & 0x80) ko = x1Adr2KnjX1t((uint16_t)((knj << 8) | code)) + ((knj & 0x40) ? 16 : 0) + (line & 15);
            else ko = (uint32_t)code * 16 + (line & 15);
            pat = L.kj[ko % X1_KANJI_SIZE];
        } else {
            pat = L.blank8 ? 0 : L.cg[(code * 8 + (line & 7)) & 0x7FF];
        }
        pb = (color & 1) ? pat : 0; pr = (color & 2) ? pat : 0; pg = (color & 4) ? pat : 0;
    }
    if (blink) { pb = ~pb; pr = ~pr; pg = ~pg; }
    op = pb | pr | pg;
    if (rev8) { pb = ~pb; pr = ~pr; pg = ~pg; op = 0xFF; }
    int n = 1;
    uint32_t tR = 0;
    if (attr & 0x80) {
        if (cx + 1 < L.xChars) {
            tR = W4[pb & 15] | (W4[pr & 15] << 8) | (W4[pg & 15] << 16) | (W4[op & 15] << 24);
            n = 2;
        }
        pb = (uint8_t)W4[pb >> 4]; pr = (uint8_t)W4[pr >> 4]; pg = (uint8_t)W4[pg >> 4]; op = (uint8_t)W4[op >> 4];
    }
    uint32_t t = pb | ((uint32_t)pr << 8) | ((uint32_t)pg << 16) | ((uint32_t)op << 24);
    if (L.key) {
        t = applyTextKey(t, L.keyPl);
        tR = applyTextKey(tR, L.keyPl);
    }
    txt[cx] = t;
    if (n == 2) txt[cx + 1] = tR;
    return n;
}

template <bool GENERIC>
static X1_FORCEINLINE void composeLine80(uint32_t *o, const uint8_t *gl, const uint32_t *txt, int n, uint8_t priNz, uint8_t priZ) {
    for (int cx = 0; cx < n; cx++) {
        const uint8_t gB = gl[cx], gR = gl[80 + cx], gG = gl[160 + cx];
        uint32_t b = E80[gB] | (E80[gR] << 1) | (E80[gG] << 2) | 0x88888888u;
        const uint32_t t = txt[cx];
        if (t) {
            uint32_t m;
            if (!GENERIC) {
                m = E80[(uint8_t)(t >> 24) & (uint8_t)~(((gB | gR | gG) & priNz) | priZ)] * 15u;
            } else {
                m = E80[t >> 24] * 15u;
                m &= ~((uint32_t)priNib[b & 255] | ((uint32_t)priNib[(b >> 8) & 255] << 8) |
                       ((uint32_t)priNib[(b >> 16) & 255] << 16) | ((uint32_t)priNib[b >> 24] << 24));
            }
            const uint32_t tt = E80[t & 255] | (E80[(t >> 8) & 255] << 1) | (E80[(t >> 16) & 255] << 2);
            b ^= (b ^ tt) & m;
        }
        o[cx] = b;
    }
}
template <bool GENERIC>
static X1_FORCEINLINE void composeLine40(uint32_t *o, const uint8_t *gl, const uint32_t *txt, int n, uint8_t priNz, uint8_t priZ) {
    for (int cx = 0; cx < n; cx++, o += 2) {
        const uint8_t gB = gl[cx], gR = gl[80 + cx], gG = gl[160 + cx];
        uint32_t b0 = E40L[gB] | (E40L[gR] << 1) | (E40L[gG] << 2) | 0x88888888u;
        uint32_t b1 = E40H[gB] | (E40H[gR] << 1) | (E40H[gG] << 2) | 0x88888888u;
        const uint32_t t = txt[cx];
        if (t) {
            uint32_t m0, m1;
            if (!GENERIC) {
                const uint8_t op = (uint8_t)(t >> 24) & (uint8_t)~(((gB | gR | gG) & priNz) | priZ);
                m0 = E40L[op] * 15u; m1 = E40H[op] * 15u;
            } else {
                m0 = E40L[t >> 24] * 15u; m1 = E40H[t >> 24] * 15u;
                m0 &= ~((uint32_t)priNib[b0 & 255] | ((uint32_t)priNib[(b0 >> 8) & 255] << 8) |
                        ((uint32_t)priNib[(b0 >> 16) & 255] << 16) | ((uint32_t)priNib[b0 >> 24] << 24));
                m1 &= ~((uint32_t)priNib[b1 & 255] | ((uint32_t)priNib[(b1 >> 8) & 255] << 8) |
                        ((uint32_t)priNib[(b1 >> 16) & 255] << 16) | ((uint32_t)priNib[b1 >> 24] << 24));
            }
            const uint8_t pb = (uint8_t)t, pr = (uint8_t)(t >> 8), pg = (uint8_t)(t >> 16);
            const uint32_t t0 = E40L[pb] | (E40L[pr] << 1) | (E40L[pg] << 2), t1 = E40H[pb] | (E40H[pr] << 1) | (E40H[pg] << 2);
            b0 ^= (b0 ^ t0) & m0;
            b1 ^= (b1 ^ t1) & m1;
        }
        o[0] = b0; o[1] = b1;
    }
}

struct RowList {
    bool valid;
    uint32_t key;
    int nAnk, nPcg, nSlow;
    uint8_t pcgUsed;
    uint8_t ankCx[80], ankCode[80], ankCol[80];
    uint8_t pcgCx[80], pcgCode[80], pcgCol[80];
    uint8_t slowCx[80];
};
static RowList rowList;
static uint32_t ankBlank[2][8];
static bool ankBlankReady;
static void buildAnkBlank(const uint8_t *cg, const uint8_t *kj) {
    for (int code = 0; code < 256; code++) {
        uint8_t a8 = 0, a16 = 0;
        for (int i = 0; i < 8; i++) a8 |= cg[code * 8 + i];
        for (int i = 0; i < 16; i++) a16 |= kj[code * 16 + i];
        if (!a8) ankBlank[0][code >> 5] |= 1u << (code & 31);
        if (!a16) ankBlank[1][code >> 5] |= 1u << (code & 31);
    }
    ankBlankReady = true;
}

static void buildRowList(const uint8_t *tv, const uint8_t *av, const uint8_t *kv, const uint8_t *cg, const uint8_t *kj,
                         int start0, int x_chars, uint32_t key) {
    RowList &R = rowList;
    R.valid = true; R.key = key;
    R.nAnk = R.nPcg = R.nSlow = 0;
    R.pcgUsed = 0;
    if (!ankBlankReady) buildAnkBlank(cg, kj);
    const uint32_t *blank = ankBlank[(x1_mode1_eff & 5) != 0];
    int cx = 0, ofs = start0;
    while (cx < x_chars) {
        const uint8_t attr = av[ofs], knj = kv[ofs], code = tv[ofs];
        if (attr < 8 && !(knj & 0x80)) {
            if (attr && !((blank[code >> 5] >> (code & 31)) & 1)) {
                R.ankCx[R.nAnk] = (uint8_t)cx; R.ankCode[R.nAnk] = code; R.ankCol[R.nAnk] = attr; R.nAnk++;
            }
            cx++;
            ofs = (ofs + 1) & 0x7FF;
        } else if ((unsigned)(attr - 0x20) < 8 && !(knj & 0x90)) {
            R.pcgUsed = 1;
            if (attr & 7) { R.pcgCx[R.nPcg] = (uint8_t)cx; R.pcgCode[R.nPcg] = code; R.pcgCol[R.nPcg] = attr & 7; R.nPcg++; }
            cx++;
            ofs = (ofs + 1) & 0x7FF;
        } else {
            R.slowCx[R.nSlow++] = (uint8_t)cx;
            const int n = ((attr & 0x80) && cx + 1 < x_chars) ? 2 : 1;
            cx += n;
            ofs = (ofs + n) & 0x7FF;
        }
    }
}

void IRAM_ATTR x1RenderLine(int y) {
    if (!tablesReady || !x1_vp_rows || !mapped) return;
    const bool hireso = mappedHireso;
    if (y >= (hireso ? 400 : 200)) return;
    uint32_t *o = (uint32_t *)rowBuf[hireso ? y : 2 * y];
    const int slot = hireso ? y : 2 * y;

    const bool col40   = !hres_mode;
    const int  maxCols = col40 ? 40 : 80;
    const int  x_chars = crtc_regs[1] < maxCols ? crtc_regs[1] : maxCols;
    const int  th      = (crtc_regs[9] & 0x1F) + 1;
    const int  row     = y / th, ty = y - row * th;
    const int  stride  = crtc_regs[1];
    if ((crtc_regs[6] && row >= crtc_regs[6]) || x_chars == 0) {
        uint32_t bk = videoGen * 0x9E3779B1u ^ 0x5BD1E995u;
        if (lineCg[slot] == bk && lineTx[slot] == 0 && lineFlags[slot] == 0x80) return;
        memset(o, 0, X1_ROW_BYTES);
        x1_lines_drawn++;
        lineCg[slot] = bk; lineTx[slot] = 0; lineFlags[slot] = 0x80;
        return;
    }
    const int rowBase = row * stride + (((crtc_regs[12] << 8) | crtc_regs[13]) & 0x7FF);
    const int start0  = rowBase & 0x7FF;
    const int page = (hireso && !(x1_mode1_eff & 2)) ? (ty & 1) : ((x1_mode1_eff & 8) ? 1 : 0);
    const int ll   = hireso ? ty >> 1 : ty;
    const int gofs = (x1_mode1_eff & 4) ? 0x400 * (ll & 15) : 0x800 * (ll & 7);

    const bool blinkPhase = (x1_frame_count & 0x10) != 0;
    const uint32_t cfg = videoGen * 0x9E3779B1u ^ (uint32_t)(crtc_regs[1] | (crtc_regs[9] << 8) | (crtc_regs[6] << 16) | (col40 << 24))
                 ^ (uint32_t)(start0 | (ty << 11) | (row << 16)) * 0x85EBCA6Bu ^ (uint32_t)((x1_mode1_eff & 0x0F) | (page << 8) | (gofs << 9)) * 0x27D4EB2Fu;
    const uint32_t cgKey = cfg * 0x01000193u + gvSum(page, gofs, start0, x_chars);
    const uint8_t fl = lineFlags[slot];
    const uint32_t ts0 = txSum(start0, x_chars);
    uint32_t tx = ts0;
    if (fl & 4) tx = tx * 0x01000193u + txSum((rowBase - stride) & 0x7FF, x_chars);
    if (fl & 1) tx ^= x1_pcg_gen * 0xC2B2AE35u;
    if ((fl & 2) && blinkPhase) tx ^= 0x165667B1u;
    if (cgKey == lineCg[slot] && tx == lineTx[slot]) return;
    uint8_t newFl = 0;

#ifdef X1_PROFILE
    uint32_t pc0 = x1Ccount();
#endif
    const bool rowDblH  = (M.avram[start0] & 0x40) != 0;
    if (rowDblH && row > 0) newFl |= 4;
    const bool blank8   = !hireso && ty >= 8 && !(x1_mode1_eff & 1);
    const uint8_t *tv = M.tvram, *av = M.avram, *kv = M.kvram;
    const uint8_t *pcgB = M.pcg[0], *pcgR = M.pcg[1], *pcgG = M.pcg[2], *cg = M.cgrom, *kj = M.kanji;
    const int lnAnk = glyphLine(0, ty, false, 0, ty), lnPcg = glyphLine(1, ty, false, 0, ty), ln16 = glyphLine(2, ty, false, 0, ty);

    uint32_t txt[80];
    memset(txt, 0, (size_t)x_chars * 4);
    {
        const uint32_t rowKey = ts0 ^ (uint32_t)(start0 | (x_chars << 12)) * 0x85EBCA6Bu ^ videoGen * 0x9E3779B1u;
        if (!rowList.valid || rowList.key != rowKey) buildRowList(tv, av, kv, cg, kj, start0, x_chars, rowKey);
        const RowList &R = rowList;
        const bool key = textKey != 0;
        const uint32_t keyPl = ((textKey & 1) ? 0xFFu : 0) | ((textKey & 2) ? 0xFF00u : 0) | ((textKey & 4) ? 0xFF0000u : 0);
        const bool ank16 = (x1_mode1_eff & 5) != 0;
        if (ank16 || !blank8) {
            const uint8_t *ank = ank16 ? kj + (ln16 & 15) : cg + (lnAnk & 7);
            const int ankStep = ank16 ? 16 : 8;
            for (int k = 0; k < R.nAnk; k++) {
                const uint32_t t = ank[R.ankCode[k] * ankStep] * ankTxtMul[R.ankCol[k]];
                if (t) txt[R.ankCx[k]] = t;
            }
        }
        if (!blank8) {
            const int l = lnPcg & 7;
            const uint8_t *pB = pcgB + l, *pR = pcgR + l, *pG = pcgG + l;
            for (int k = 0; k < R.nPcg; k++) {
                const int i = R.pcgCode[k] * 8;
                uint32_t t = (pB[i] | ((uint32_t)pR[i] << 8) | ((uint32_t)pG[i] << 16)) & colMask[R.pcgCol[k]];
                t |= (uint32_t)(uint8_t)(t | (t >> 8) | (t >> 16)) << 24;
                if (key) t = applyTextKey(t, keyPl);
                if (t) txt[R.pcgCx[k]] = t;
            }
        }
        uint8_t attrOr = 0;
        if (R.nSlow) {
            TextLine L;
            L.tv = tv; L.av = av; L.kv = kv; L.pcgB = pcgB; L.pcgR = pcgR; L.pcgG = pcgG; L.cg = cg; L.kj = kj;
            L.rowBase = rowBase; L.stride = stride; L.row = row; L.th = th; L.ty = ty; L.xChars = x_chars;
            L.lnAnk = lnAnk; L.lnPcg = lnPcg; L.ln16 = ln16;
            L.key = key; L.keyPl = keyPl;
            L.rowDblH = rowDblH; L.blank8 = blank8; L.blinkPhase = blinkPhase;
            L.attrOr = 0;
            for (int k = 0; k < R.nSlow; k++) textCellSlow(L, R.slowCx[k], txt);
            attrOr = L.attrOr;
        }
        newFl |= R.pcgUsed | ((attrOr >> 5) & 1) | ((attrOr >> 3) & 2);
    }
    uint32_t txNew = ts0;
    if (newFl & 4) txNew = txNew * 0x01000193u + txSum((rowBase - stride) & 0x7FF, x_chars);
    if (newFl & 1) txNew ^= x1_pcg_gen * 0xC2B2AE35u;
    if ((newFl & 2) && blinkPhase) txNew ^= 0x165667B1u;
    uint32_t txtHash = 0;
    if (cgKey == lineCg[slot]) {
        txtHash = 2166136261u;
        for (int cx = 0; cx < x_chars; cx++) if (txt[cx]) txtHash = mixWord(txtHash, txt[cx] ^ (uint32_t)(cx + 1) * 0x9E3779B1u);
        txtHash |= 1;
    }
#ifdef X1_PROFILE
    uint32_t pc1 = x1Ccount();
    x1_prof_txt += pc1 - pc0;
#endif
    if (txtHash && txtHash == lineTxt[slot]) {
        lineTx[slot] = txNew; lineFlags[slot] = newFl;
        x1_lines_txtsame++;
        return;
    }
    x1_lines_drawn++;

    uint8_t gl[240];
    uint8_t *const lineB = gl, *const lineR = gl + 80, *const lineG = gl + 160;
    {
        const uint8_t *gp = M.gvram + page * GVRAM_PAGE_SZ;
        if (!(gofs & 0x400)) {
            const int first = 0x800 - start0;
            const int n1 = first >= x_chars ? x_chars : first;
            memcpy(lineB, gp + GVRAM_B_OFS + gofs + start0, n1);
            memcpy(lineR, gp + GVRAM_R_OFS + gofs + start0, n1);
            memcpy(lineG, gp + GVRAM_G_OFS + gofs + start0, n1);
            if (n1 < x_chars) {
                memcpy(lineB + n1, gp + GVRAM_B_OFS + gofs, x_chars - n1);
                memcpy(lineR + n1, gp + GVRAM_R_OFS + gofs, x_chars - n1);
                memcpy(lineG + n1, gp + GVRAM_G_OFS + gofs, x_chars - n1);
            }
        } else {
            for (int cx = 0; cx < x_chars; cx++) {
                int a = (gofs | ((start0 + cx) & 0x7FF)) & 0x3FFF;
                lineB[cx] = gp[GVRAM_B_OFS + a]; lineR[cx] = gp[GVRAM_R_OFS + a]; lineG[cx] = gp[GVRAM_G_OFS + a];
            }
        }
    }
#ifdef X1_PROFILE
    uint32_t pc2 = x1Ccount();
#endif

    const uint8_t pri = priority_reg;
    const bool priGeneric = pri != 0 && (pri & 0xFE) != 0xFE;
    const uint8_t priNz = pri ? 0xFF : 0, priZ = (pri & 1) ? 0xFF : 0;
    if (col40) {
        if (priGeneric) composeLine40<true>(o, gl, txt, x_chars, priNz, priZ);
        else composeLine40<false>(o, gl, txt, x_chars, priNz, priZ);
    } else {
        if (priGeneric) composeLine80<true>(o, gl, txt, x_chars, priNz, priZ);
        else composeLine80<false>(o, gl, txt, x_chars, priNz, priZ);
    }
    {
        int used = x_chars * (col40 ? 2 : 1);
        for (int i = used; i < X1_ROW_BYTES / 4; i++) o[i] = 0;
    }
    lineCg[slot] = cgKey; lineTx[slot] = txNew; lineTxt[slot] = txtHash; lineFlags[slot] = newFl;
#ifdef X1_PROFILE
    uint32_t pc3 = x1Ccount();
    x1_prof_rd  += pc2 - pc1;
    x1_prof_bmp += pc3 - pc2;
#endif
}

#ifdef X1_PROFILE
uint32_t x1_prof_rd = 0, x1_prof_bmp = 0, x1_prof_txt = 0;

void x1MemBench() {
    if (!tablesReady || !x1_vp_rows) return;
    uint8_t keep[18];
    memcpy(keep, crtc_regs, sizeof(keep));
    bool keepHres = hres_mode, keepHireso = x1_hireso;
    uint8_t keepMode1 = x1_mode1_eff;
    crtc_regs[1] = 80; crtc_regs[6] = 25; crtc_regs[9] = 15; crtc_regs[12] = crtc_regs[13] = 0;
    hres_mode = true; x1_mode1_eff = 0x05; x1_hireso = true;
    mapRows(true);
    uint32_t c0 = x1Ccount();
    for (int k = 0; k < 5; k++) {
        videoGen++;
        for (int y = 0; y < 400; y++) x1RenderLine(y);
    }
    uint32_t t400 = (x1Ccount() - c0) / 5;
    memcpy(crtc_regs, keep, sizeof(keep));
    hres_mode = keepHres; x1_hireso = keepHireso; x1_mode1_eff = keepMode1;
    mapRows(x1_hireso);
    Serial.printf("BENCH render 640x400 all lines: %u us/frame\n", (unsigned)(t400 / 240));
    x1_prof_rd = x1_prof_bmp = x1_prof_txt = 0;
}
#endif
