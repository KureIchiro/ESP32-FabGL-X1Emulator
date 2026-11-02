// ============================================================================
//  X1 video renderer: テキスト+PCG描画 / ビットマップ描画 / 優先度合成
//  無印X1はdisp_bank(ダブルバッファ)が存在しないため常にビットマップ側は
//  単一バンクとして扱う。
//
//  【描画の組み立て】(2026-10-08 作り直し)
//  1文字(8ドット)ずつ、ビットマップとテキスト/PCGと優先度をレジスタの上で
//  合成して、ビューポートへ1回だけ書く(以前はビットマップを書いてから
//  テキストで読み書きし、優先度があればさらにもう1回読み書きしていた)。
//    * 左右2ドットの組を1つの番号(左の色 | 右の色<<3、0〜63)にし、
//      番号 → 出力ワード の64要素の表を引く。SP[v] はプレーン1枚の
//      バイト v から4組ぶんの番号の素を作る(B | R<<1 | G<<2 で番号になる)。
//    * 40桁(320ドット)は1組 = 出力4画素 = 1ワード、80桁は1組 = 2画素 = 半ワード。
//    * パレット・優先度が変わったら表を作り直す(パレットの書き込みは1フレームに数回)。
//  描画関数は IRAM に置く(フラッシュのキャッシュは PSRAM と共用なので、
//  GVRAM や PSRAM 上の RAM を読むたびに描画コードが追い出されていた)。
//
//  【FabGLのピクセル並び】vgabasecontroller.h:279-292 より、1ワード(4px)内は
//    pixel0=byte2, pixel1=byte3, pixel2=byte0, pixel3=byte1
//  である。またネイティブ1バイトには同期信号(VSync/HSync)ビットが含まれる
//  ため、パレット値は必ず x1MakeNativeColor() 経由で作ること。0x00を書くと
//  同期が飛ぶ。
// ============================================================================
// 【最適化】Arduinoのesp32コアは既定で -Os。描画ループ はフレームごとに
// 数万回まわるため、この翻訳単位だけ O3 に引き上げる。
// (X1.ino:20 の #pragma は .ino の翻訳単位にしか効かない)
#pragma GCC optimize("O3")

#include "x1.h"
#include <string.h>

#ifdef _MSC_VER
#define X1_NOINLINE    __declspec(noinline)
#define X1_FORCEINLINE __forceinline
#else
#define X1_NOINLINE    __attribute__((noinline))
#define X1_FORCEINLINE inline __attribute__((always_inline))
#endif

static uint32_t SP[256];
static uint8_t  pairColors[64];
static uint32_t bmpD[64], txD[64];
static uint16_t bmpH[64], txH[64];
static uint32_t opW40[4], opW80[16];
static uint8_t  W4[16];
static uint32_t colMask[8];
static uint32_t ankColIdx[8];
static uint32_t blackW;
static bool tablesReady = false;
static volatile bool palDirty = true;

static uint8_t priFront[8];
static bool    priFast;
static uint8_t priNzM, priZM;
static uint32_t priTA, priTBA, priTC, priTDC;
static uint8_t textKey;

void x1VideoBuildTables() {
    for (int v = 0; v < 256; v++) {
        uint32_t w = 0;
        for (int j = 0; j < 4; j++) {
            uint32_t l = (v >> (7 - 2 * j)) & 1, r = (v >> (6 - 2 * j)) & 1;
            w |= (l | (r << 3)) << (8 * j);
        }
        SP[v] = w;
    }
    for (int i = 0; i < 64; i++) pairColors[i] = (uint8_t)((1 << (i & 7)) | (1 << (i >> 3)));
    for (int x = 0; x < 4; x++) opW40[x] = ((x & 2) ? 0xFFFF0000u : 0) | ((x & 1) ? 0x0000FFFFu : 0);
    for (int x = 0; x < 16; x++)
        opW80[x] = ((x & 8) ? 0x00FF0000u : 0) | ((x & 4) ? 0xFF000000u : 0) | ((x & 2) ? 0x000000FFu : 0) | ((x & 1) ? 0x0000FF00u : 0);
    for (int v = 0; v < 16; v++) {
        uint8_t w = 0;
        for (int k = 0; k < 4; k++) if ((v >> (3 - k)) & 1) w |= (uint8_t)(0xC0 >> (2 * k));
        W4[v] = w;
    }
    for (int c = 0; c < 8; c++)
        colMask[c] = ((c & 1) ? 0xFFu : 0) | ((c & 2) ? 0xFF00u : 0) | ((c & 4) ? 0xFF0000u : 0);
    palDirty = true;
    tablesReady = true;
}

void x1VideoPaletteChanged() { palDirty = true; }

void x1VideoUpdatePriority() {
    for (int c = 0; c < 8; c++) priFront[c] = (priority_reg & (1 << c)) ? 0xFF : 0x00;
    textKey = ((x1_mode2 & 0x08) && (x1_mode2 & 7)) ? (uint8_t)(8 | (x1_mode2 & 7)) : 0;
    bool allNz = true, noNz = true;
    for (int c = 1; c < 8; c++) { if (priFront[c]) noNz = false; else allNz = false; }
    priFast = allNz || noNz;
    priNzM = allNz ? 0xFF : 0;
    priZM = priFront[0];
    priTA  = priFront[0] | ((uint32_t)priFront[4] << 8);
    priTBA = (priFront[0] ^ priFront[1]) | ((uint32_t)(priFront[4] ^ priFront[5]) << 8);
    priTC  = priFront[2] | ((uint32_t)priFront[6] << 8);
    priTDC = (priFront[2] ^ priFront[3]) | ((uint32_t)(priFront[6] ^ priFront[7]) << 8);
    for (int c = 0; c < 8; c++) ankColIdx[c] = (textKey && c == (textKey & 7) ? 0u : (uint32_t)c) * 0x09090909u;
    palDirty = true;
}

uint32_t x1_vram_gen = 0;
uint32_t x1_gv_gen[8][128];
uint32_t x1_tx_gen[128];
uint32_t x1_pcg_gen = 0;
uint32_t x1_lines_drawn = 0;
uint32_t x1_lines_txtsame = 0;
static uint32_t videoGen = 1;
static uint32_t lineCg[X1_SCREEN_H], lineTx[X1_SCREEN_H], lineTxt[X1_SCREEN_H];
static uint8_t  lineFlags[X1_SCREEN_H];
static uint32_t colorGen[8], colorGenCnt = 0;
static uint8_t  lineColors[X1_SCREEN_H];
static uint32_t lineColorKey[X1_SCREEN_H];
static uint8_t  palShown[8], fixShown[8], priShown;
static bool     shownValid = false;

void x1VideoInvalidate() { videoGen++; }

static inline uint32_t colorKey(uint8_t mask) {
    uint32_t k = 0;
    for (int c = 0; mask; c++, mask >>= 1) if (mask & 1) k = k * 0x01000193u + colorGen[c];
    return k;
}

static inline uint32_t mixWord(uint32_t h, uint32_t w) { h = (h ^ w) * 0x9E3779B1u; return h ^ (h >> 16); }

static inline uint32_t genSum(const uint32_t *gen, int start, int n) {
    uint32_t s = 0;
    int c = start >> 4, cnt = ((start & 15) + n + 15) >> 4;
    for (int i = 0; i < cnt; i++) s = s * 0x01000193u + gen[(c + i) & 127];
    return s;
}

static const uint32_t PALTRACK_FRAMES = 120;
static uint32_t palTrackUntil = 0;
static uint32_t lineColorStamp[X1_SCREEN_H];
static inline bool colorsUnchanged(int y) {
    if (lineColorStamp[y] == colorGenCnt) return true;
    if (colorKey(lineColors[y]) != lineColorKey[y]) return false;
    lineColorStamp[y] = colorGenCnt;
    return true;
}
static void rebuildPalTables() {
    palDirty = false;
    if (!shownValid || priShown != priority_reg || memcmp(fixShown, fixed_palette_native, 8) != 0) videoGen++;
    else for (int c = 0; c < 8; c++)
        if (palShown[c] != palette_native[c]) { colorGen[c] = ++colorGenCnt; palTrackUntil = x1_frame_count + PALTRACK_FRAMES; }
    memcpy(palShown, palette_native, 8);
    memcpy(fixShown, fixed_palette_native, 8);
    priShown = priority_reg;
    shownValid = true;
    for (int i = 0; i < 64; i++) {
        int l = i & 7, r = i >> 3;
        uint32_t bl = palette_native[l], br = palette_native[r];
        uint32_t tl = fixed_palette_native[l], tr = fixed_palette_native[r];
        bmpD[i] = br | (br << 8) | (bl << 16) | (bl << 24);
        txD[i]  = tr | (tr << 8) | (tl << 16) | (tl << 24);
        bmpH[i] = (uint16_t)(bl | (br << 8));
        txH[i]  = (uint16_t)(tl | (tr << 8));
    }
    blackW = (uint32_t)x1MakeNativeColor(0, 0, 0) * 0x01010101u;
}

struct TextLine {
    const uint8_t *tv, *av, *kv, *pcgB, *pcgR, *pcgG, *cg;
    int rowBase, stride, row, ty, xChars;
    bool rowDblH, blankLine, blinkPhase;
    uint8_t attrOr;
};

static inline uint32_t dotColor(uint32_t ti, int k) {
    const uint32_t idx = (ti >> (8 * (k >> 1))) & 63;
    return (k & 1) ? (idx >> 3) : (idx & 7);
}

static int IRAM_ATTR X1_NOINLINE textCellSlow(TextLine &L, int cx, uint8_t *txo, uint32_t *txi) {
    const uint16_t ofs = (L.rowBase + cx) & 0x7FF;
    const uint8_t code = L.tv[ofs], attr = L.av[ofs], knj = L.kv[ofs];
    L.attrOr |= attr;
    const bool blink = (attr & 0x10) && L.blinkPhase;
    const bool rev8  = (attr & 0x08) != 0;
    const bool isPcg = (attr & 0x20) != 0;
    int yi = L.ty;
    if ((attr & 0x40) && L.rowDblH) {
        int dy = 0;
        if (L.row > 0) {
            uint16_t pofs = (L.rowBase - L.stride + cx) & 0x7FF;
            if (L.tv[pofs] == code && L.av[pofs] == attr) dy = 8;
        }
        yi = (L.ty + dy) >> 1;
    }
    const int line = (code * X1_CHAR_H + yi) & 0x7FF;
    int color = attr & 7;
    uint8_t op;
    uint32_t ti;
    if (isPcg) {
        int pl = line;
        if (knj & 0x90) pl = ((code & 0xFE) * 8 + (yi & 7)) & 0x7FF;
        uint8_t pb = (color & 1) && !L.blankLine ? L.pcgB[pl] : 0;
        uint8_t pr = (color & 2) && !L.blankLine ? L.pcgR[pl] : 0;
        uint8_t pg = (color & 4) && !L.blankLine ? L.pcgG[pl] : 0;
        if (blink) { pb = ~pb; pr = ~pr; pg = ~pg; }
        op = pb | pr | pg;
        if (rev8) { pb = ~pb; pr = ~pr; pg = ~pg; op = 0xFF; }
        if (textKey) {
            const uint8_t kb = (textKey & 1) ? 0xFF : 0, kr = (textKey & 2) ? 0xFF : 0, kg = (textKey & 4) ? 0xFF : 0;
            uint8_t km = op & (uint8_t)~((pb ^ kb) | (pr ^ kr) | (pg ^ kg));
            pb &= (uint8_t)~km; pr &= (uint8_t)~km; pg &= (uint8_t)~km;
        }
        ti = SP[pb] | (SP[pr] << 1) | (SP[pg] << 2);
    } else {
        uint8_t pat;
        if (knj & 0x80) {
            uint32_t ko = x1Adr2KnjX1t((uint16_t)((knj << 8) | code));
            pat = (color == 0 && !rev8) || ko >= X1_KANJI_SIZE ? 0
                : M.kanji[(ko + ((knj & 0x40) ? 16 : 0) + ((yi << 1) & 15)) % X1_KANJI_SIZE];
        } else {
            pat = (L.blankLine || (color == 0 && !rev8)) ? 0 : L.cg[line];
        }
        op = (rev8 != blink) ? (uint8_t)~pat : pat;
        if (textKey && color == (textKey & 7)) color = 0;
        ti = (uint32_t)color * 0x09090909u;
    }

    if (attr & 0x80) {
        uint32_t tl = 0, tr = 0;
        for (int p = 0; p < 4; p++) {
            tl |= dotColor(ti, p) * 9u << (8 * p);
            tr |= dotColor(ti, 4 + p) * 9u << (8 * p);
        }
        txo[cx] = W4[op >> 4]; txi[cx] = tl;
        if (cx + 1 < L.xChars) { txo[cx + 1] = W4[op & 15]; txi[cx + 1] = tr; return 2; }
        return 1;
    }
    txo[cx] = op; txi[cx] = ti;
    return 1;
}

template <bool TRACK>
static X1_FORCEINLINE void bmpLine80(uint32_t *o, const uint8_t *gl, int n, uint32_t &orw, uint32_t &andw) {
    for (int cx = 0; cx < n; cx++, o += 2) {
        const uint32_t bi = SP[gl[cx]] | (SP[gl[80 + cx]] << 1) | (SP[gl[160 + cx]] << 2);
        if (TRACK) { orw |= bi; andw &= bi; }
        o[0] = ((uint32_t)bmpH[bi & 63] << 16) | bmpH[(bi >> 8) & 63];
        o[1] = ((uint32_t)bmpH[(bi >> 16) & 63] << 16) | bmpH[bi >> 24];
    }
}
template <bool TRACK>
static X1_FORCEINLINE void bmpLine40(uint32_t *o, const uint8_t *gl, int n, uint32_t &orw, uint32_t &andw) {
    for (int cx = 0; cx < n; cx++, o += 4) {
        const uint32_t bi = SP[gl[cx]] | (SP[gl[80 + cx]] << 1) | (SP[gl[160 + cx]] << 2);
        if (TRACK) { orw |= bi; andw &= bi; }
        o[0] = bmpD[bi & 63]; o[1] = bmpD[(bi >> 8) & 63]; o[2] = bmpD[(bi >> 16) & 63]; o[3] = bmpD[bi >> 24];
    }
}
static X1_FORCEINLINE uint32_t priFrontBits(uint32_t gB, uint32_t gR, uint32_t gG,
                                            uint32_t ta, uint32_t tba, uint32_t tc, uint32_t tdc) {
    const uint32_t b2 = gB * 0x0101u, r2 = gR * 0x0101u;
    const uint32_t x0 = ta ^ (b2 & tba), x1 = tc ^ (b2 & tdc);
    const uint32_t f = x0 ^ (r2 & (x1 ^ x0));
    return f ^ (gG & ((f >> 8) ^ f));
}
template <bool GENERIC>
static X1_FORCEINLINE void overlay80(uint32_t *o, const uint8_t *gl, const uint8_t *vcx, const uint8_t *vop, const uint32_t *vti,
                                     int nv, uint32_t nzM, uint32_t zM) {
    const uint32_t ta = priTA, tba = priTBA, tc = priTC, tdc = priTDC;
    for (int k = 0; k < nv; k++) {
        const int cx = vcx[k];
        const uint32_t op = vop[k];
        const uint8_t gB = gl[cx], gR = gl[80 + cx], gG = gl[160 + cx];
        uint32_t m8;
        if (!GENERIC) {
            const uint32_t any = gB | gR | gG;
            m8 = op & ~((any & nzM) | (~any & zM));
        } else {
            m8 = op & ~priFrontBits(gB, gR, gG, ta, tba, tc, tdc) & 0xFF;
        }
        const uint32_t m0 = opW80[m8 >> 4], m1 = opW80[m8 & 15];
        const uint32_t ti = vti[k];
        const uint32_t t0 = ((uint32_t)txH[ti & 63] << 16) | txH[(ti >> 8) & 63];
        const uint32_t t1 = ((uint32_t)txH[(ti >> 16) & 63] << 16) | txH[ti >> 24];
        uint32_t *w = o + 2 * cx;
        const uint32_t w0 = w[0], w1 = w[1];
        w[0] = w0 ^ ((w0 ^ t0) & m0);
        w[1] = w1 ^ ((w1 ^ t1) & m1);
    }
}
template <bool GENERIC>
static X1_FORCEINLINE void overlay40(uint32_t *o, const uint8_t *gl, const uint8_t *vcx, const uint8_t *vop, const uint32_t *vti,
                                     int nv, uint32_t nzM, uint32_t zM) {
    const uint32_t ta = priTA, tba = priTBA, tc = priTC, tdc = priTDC;
    for (int k = 0; k < nv; k++) {
        const int cx = vcx[k];
        const uint8_t gB = gl[cx], gR = gl[80 + cx], gG = gl[160 + cx];
        uint32_t m8 = vop[k];
        if (!GENERIC) {
            const uint32_t any = gB | gR | gG;
            m8 &= ~((any & nzM) | (~any & zM));
        } else {
            m8 &= ~priFrontBits(gB, gR, gG, ta, tba, tc, tdc);
        }
        const uint32_t ti = vti[k];
        uint32_t *w = o + 4 * cx;
        for (int j = 0; j < 4; j++) {
            const uint32_t m = opW40[(m8 >> (6 - 2 * j)) & 3];
            const uint32_t x = w[j];
            w[j] = x ^ ((x ^ txD[(ti >> (8 * j)) & 63]) & m);
        }
    }
}

struct RowList {
    bool valid;
    uint32_t key;
    int nAnk, nPcg, nSlow;
    uint8_t pcgUsed, slowAttrOr;
    uint8_t ankCx[80], ankCode[80], ankCol[80];
    uint8_t pcgCx[80], pcgCode[80], pcgCol[80];
    uint8_t slowCx[80];
};
static RowList rowList;
static uint8_t  visCx[80], visOp[80];
static uint32_t visTi[80];
static uint8_t  slowOp[80];
static uint32_t slowTi[80];
static uint32_t lineBuf[X1_SCREEN_W / 4];

static void buildRowList(const uint8_t *tv, const uint8_t *av, const uint8_t *kv, const uint8_t *cg,
                         int start0, int x_chars, uint32_t key) {
    RowList &R = rowList;
    R.valid = true; R.key = key;
    R.nAnk = R.nPcg = R.nSlow = 0;
    R.pcgUsed = 0; R.slowAttrOr = 0;
    int cx = 0, ofs = start0;
    while (cx < x_chars) {
        const uint8_t attr = av[ofs], knj = kv[ofs], code = tv[ofs];
        if (attr < 8 && !(knj & 0x80)) {
            const uint8_t *g = cg + code * X1_CHAR_H;
            if (attr && (g[0] | g[1] | g[2] | g[3] | g[4] | g[5] | g[6] | g[7])) {
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
            R.slowAttrOr |= attr;
            R.slowCx[R.nSlow++] = (uint8_t)cx;
            const int n = ((attr & 0x80) && cx + 1 < x_chars) ? 2 : 1;
            cx += n;
            ofs = (ofs + n) & 0x7FF;
        }
    }
}

static inline void copyWrap(uint8_t *dst, const uint8_t *plane, int start, int n) {
    int first = 0x800 - start;
    if (first >= n) { memcpy(dst, plane + start, n); return; }
    memcpy(dst, plane + start, first);
    memcpy(dst + first, plane, n - first);
}

void IRAM_ATTR x1RenderLine(int y) {
    if (!tablesReady || !x1_scanline_rows) return;
    uint8_t *dst = x1_scanline_rows[y];
    if (!dst) return;
    if (palDirty) rebuildPalTables();
    uint32_t *o = (uint32_t *)dst;

    const bool dbl     = !hres_mode;
    const int  wpc     = dbl ? 4 : 2;
    const bool hasPri  = (priority_reg != 0);
    const int maxCols = dbl ? 40 : 80;
    const int x_chars = crtc_regs[1] < maxCols ? crtc_regs[1] : maxCols;
    const int th      = (crtc_regs[9] & 0x1F) + 1;
    const int row     = y / th, ty = y - row * th;
    const int stride  = crtc_regs[1];
    if ((crtc_regs[6] && row >= crtc_regs[6]) || x_chars == 0) {
        uint32_t bk = videoGen * 0x9E3779B1u ^ 0x5BD1E995u;
        if (lineCg[y] == bk && lineTx[y] == 0 && lineFlags[y] == 0x80) return;
        for (int i = 0; i < X1_SCREEN_W / 4; i++) o[i] = blackW;
        x1_lines_drawn++;
        lineCg[y] = bk; lineTx[y] = 0; lineFlags[y] = 0x80;
        lineColors[y] = 0; lineColorKey[y] = 0; lineColorStamp[y] = colorGenCnt;
        return;
    }
    const int rowBase  = row * stride + (((crtc_regs[12] << 8) | crtc_regs[13]) & 0x7FF);
    const int planeOfs = (ty * 0x800) & 0x3FFF;

    const bool blinkPhase = (x1_frame_count & 0x10) != 0;
    const int  start0 = rowBase & 0x7FF;
    const uint32_t cfg = videoGen * 0x9E3779B1u ^ (uint32_t)(crtc_regs[1] | (crtc_regs[9] << 8) | (crtc_regs[6] << 16) | (dbl << 24))
                 ^ (uint32_t)(start0 | (ty << 12)) * 0x85EBCA6Bu;
    const uint32_t cgKey = cfg * 0x01000193u + genSum(x1_gv_gen[planeOfs >> 11], start0, x_chars);
    const uint8_t fl = lineFlags[y];
    const uint32_t txSum = genSum(x1_tx_gen, start0, x_chars);
    uint32_t tx = txSum;
    if (fl & 4) tx = tx * 0x01000193u + genSum(x1_tx_gen, (rowBase - stride) & 0x7FF, x_chars);
    if (fl & 1) tx ^= x1_pcg_gen * 0xC2B2AE35u;
    if ((fl & 2) && blinkPhase) tx ^= 0x27D4EB2Fu;
    if (cgKey == lineCg[y] && tx == lineTx[y] && colorsUnchanged(y)) return;
    uint8_t newFl = 0;
    const bool track = (int32_t)(palTrackUntil - x1_frame_count) > 0;
    uint8_t cm = 0xFF;
    uint32_t orw = 0, andw = 0xFFFFFFFFu;

#ifdef X1_PROFILE
    uint32_t pc0 = x1Ccount();
#endif
    const bool rowDblH    = (M.avram[rowBase & 0x7FF] & 0x40) != 0;
    if (rowDblH && row > 0) newFl |= 4;
    const bool blankLine  = ty >= 8;
    const uint8_t *tv = M.tvram, *av = M.avram, *kv = M.kvram;
    const uint8_t *pcgB = M.pcg[0], *pcgR = M.pcg[1], *pcgG = M.pcg[2], *cg = M.cgrom;

    const uint32_t rowKey = txSum ^ (uint32_t)(start0 | (x_chars << 12)) * 0x85EBCA6Bu ^ videoGen * 0x9E3779B1u;
    if (!rowList.valid || rowList.key != rowKey) buildRowList(tv, av, kv, cg, start0, x_chars, rowKey);
    const RowList &R = rowList;
    newFl |= R.pcgUsed | ((R.slowAttrOr >> 5) & 1) | ((R.slowAttrOr >> 3) & 2);
    int nv = 0;
    if (!blankLine) {
        const uint8_t *ank = cg + ty;
        for (int k = 0; k < R.nAnk; k++) {
            const uint8_t pat = ank[R.ankCode[k] * X1_CHAR_H];
            if (pat) { visCx[nv] = R.ankCx[k]; visOp[nv] = pat; visTi[nv] = ankColIdx[R.ankCol[k]]; nv++; }
        }
        const uint8_t *pB = pcgB + ty, *pR = pcgR + ty, *pG = pcgG + ty;
        const uint8_t key = textKey;
        if (!key) {
            for (int k = 0; k < R.nPcg; k++) {
                const int i = R.pcgCode[k] * X1_CHAR_H;
                const uint32_t t = (pB[i] | ((uint32_t)pR[i] << 8) | ((uint32_t)pG[i] << 16)) & colMask[R.pcgCol[k]];
                if (!t) continue;
                visCx[nv] = R.pcgCx[k]; visOp[nv] = (uint8_t)(t | (t >> 8) | (t >> 16));
                visTi[nv] = SP[t & 0xFF] | (SP[(t >> 8) & 0xFF] << 1) | (SP[t >> 16] << 2); nv++;
            }
        } else {
            for (int k = 0; k < R.nPcg; k++) {
                const int i = R.pcgCode[k] * X1_CHAR_H;
                const uint32_t t = (pB[i] | ((uint32_t)pR[i] << 8) | ((uint32_t)pG[i] << 16)) & colMask[R.pcgCol[k]];
                uint8_t pb = (uint8_t)t, pr = (uint8_t)(t >> 8), pg = (uint8_t)(t >> 16);
                const uint8_t op = pb | pr | pg;
                if (!op) continue;
                const uint8_t kb = (key & 1) ? 0xFF : 0, kr = (key & 2) ? 0xFF : 0, kg = (key & 4) ? 0xFF : 0;
                const uint8_t km = op & (uint8_t)~((pb ^ kb) | (pr ^ kr) | (pg ^ kg));
                pb &= (uint8_t)~km; pr &= (uint8_t)~km; pg &= (uint8_t)~km;
                visCx[nv] = R.pcgCx[k]; visOp[nv] = op; visTi[nv] = SP[pb] | (SP[pr] << 1) | (SP[pg] << 2); nv++;
            }
        }
    }
    if (R.nSlow) {
        TextLine L;
        L.tv = tv; L.av = av; L.kv = kv; L.pcgB = pcgB; L.pcgR = pcgR; L.pcgG = pcgG; L.cg = cg;
        L.rowBase = rowBase; L.stride = stride; L.row = row; L.ty = ty; L.xChars = x_chars;
        L.rowDblH = rowDblH; L.blankLine = blankLine; L.blinkPhase = blinkPhase;
        L.attrOr = 0;
        for (int k = 0; k < R.nSlow; k++) {
            const int cx = R.slowCx[k];
            const int n = textCellSlow(L, cx, slowOp, slowTi);
            for (int j = cx; j < cx + n; j++)
                if (slowOp[j]) { visCx[nv] = (uint8_t)j; visOp[nv] = slowOp[j]; visTi[nv] = slowTi[j]; nv++; }
        }
    }
    uint32_t txNew = txSum;
    if (newFl & 4) txNew = txNew * 0x01000193u + genSum(x1_tx_gen, (rowBase - stride) & 0x7FF, x_chars);
    if (newFl & 1) txNew ^= x1_pcg_gen * 0xC2B2AE35u;
    if ((newFl & 2) && blinkPhase) txNew ^= 0x27D4EB2Fu;
    uint32_t txtHash = 0;
    if (cgKey == lineCg[y] && colorsUnchanged(y)) {
        txtHash = 2166136261u ^ (uint32_t)nv;
        for (int k = 0; k < nv; k++) {
            txtHash = mixWord(txtHash, visCx[k] | ((uint32_t)visOp[k] << 8));
            txtHash = mixWord(txtHash, visTi[k]);
        }
        txtHash |= 1;
    }
#ifdef X1_PROFILE
    uint32_t pc1 = x1Ccount();
    x1_prof_txt += pc1 - pc0;
#endif
    if (txtHash && txtHash == lineTxt[y]) {
        lineTx[y] = txNew; lineFlags[y] = newFl;
        x1_lines_txtsame++;
        return;
    }
    x1_lines_drawn++;

    uint8_t gl[240];
    uint8_t *const lineB = gl, *const lineR = gl + 80, *const lineG = gl + 160;
    {
        int start = rowBase & 0x7FF;
        const uint8_t *gp = M.gvram + ((x1_mode1 & 0x08) ? GVRAM_PAGE_SZ : 0);
        copyWrap(lineB, &gp[GVRAM_B_OFS + planeOfs], start, x_chars);
        copyWrap(lineR, &gp[GVRAM_R_OFS + planeOfs], start, x_chars);
        copyWrap(lineG, &gp[GVRAM_G_OFS + planeOfs], start, x_chars);
    }
#ifdef X1_PROFILE
    uint32_t pc2 = x1Ccount();
#endif

    const bool priGeneric = hasPri && !priFast;
    const uint32_t nzM = hasPri ? priNzM : 0, zM = hasPri ? priZM : 0;
    uint32_t *const out = nv ? lineBuf : o;
    if (dbl) {
        if (track) bmpLine40<true>(out, gl, x_chars, orw, andw); else bmpLine40<false>(out, gl, x_chars, orw, andw);
        if (nv) {
            if (priGeneric) overlay40<true>(out, gl, visCx, visOp, visTi, nv, nzM, zM);
            else overlay40<false>(out, gl, visCx, visOp, visTi, nv, nzM, zM);
        }
    } else {
        if (track) bmpLine80<true>(out, gl, x_chars, orw, andw); else bmpLine80<false>(out, gl, x_chars, orw, andw);
        if (nv) {
            if (priGeneric) overlay80<true>(out, gl, visCx, visOp, visTi, nv, nzM, zM);
            else overlay80<false>(out, gl, visCx, visOp, visTi, nv, nzM, zM);
        }
    }
    if (nv) memcpy(o, lineBuf, (size_t)x_chars * wpc * 4);
    for (int i = x_chars * wpc; i < X1_SCREEN_W / 4; i++) o[i] = blackW;
    lineCg[y] = cgKey; lineTx[y] = txNew; lineTxt[y] = txtHash; lineFlags[y] = newFl;
    if (track) {
        uint32_t ob = 0, ab = 7;
        for (int j = 0; j < 4; j++) {
            uint32_t xo = (orw >> (8 * j)) & 63, xa = (andw >> (8 * j)) & 63;
            ob |= (xo & 7) | (xo >> 3);
            ab &= (xa & 7) & (xa >> 3);
        }
        cm = 0;
        for (int col = 0; col < 8; col++) if (!(col & ~ob) && (col & ab) == ab) cm |= (uint8_t)(1 << col);
    }
    lineColors[y] = cm; lineColorKey[y] = colorKey(cm); lineColorStamp[y] = colorGenCnt;
#ifdef X1_PROFILE
    uint32_t pc3 = x1Ccount();
    x1_prof_rd  += pc2 - pc1;
    x1_prof_bmp += pc3 - pc2;
#endif
}

#ifdef X1_PROFILE
uint32_t x1_prof_rd = 0, x1_prof_bmp = 0, x1_prof_txt = 0;

void x1MemBench() {
    if (!tablesReady || !x1_scanline_rows) return;
    static uint32_t scratch[160];
    const uint32_t fill = (uint32_t)x1MakeNativeColor(0, 0, 0) * 0x01010101u;
    volatile uint32_t sink = 0;
    uint32_t c0, t_vs, t_is, t_rmw, t_gv, t_rd;

    c0 = x1Ccount();
    for (int y = 0; y < X1_SCREEN_H; y++) {
        uint32_t *d = (uint32_t *)x1_scanline_rows[y];
        for (int i = 0; i < 160; i++) d[i] = fill;
    }
    t_vs = x1Ccount() - c0;

    c0 = x1Ccount();
    for (int y = 0; y < X1_SCREEN_H; y++) {
        for (int i = 0; i < 160; i++) scratch[i] = fill + y;
        sink += scratch[y & 127];
    }
    t_is = x1Ccount() - c0;

    c0 = x1Ccount();
    for (int y = 0; y < X1_SCREEN_H; y++) {
        uint32_t *d = (uint32_t *)x1_scanline_rows[y];
        for (int i = 0; i < 160; i++) d[i] = d[i] | fill;
    }
    t_rmw = x1Ccount() - c0;

    c0 = x1Ccount();
    {
        uint32_t s = 0;
        for (int y = 0; y < X1_SCREEN_H; y++) {
            int row_ofs_base = (y >> 3) * 80;
            int plane_ofs = ((y & 7) * 0x800) & 0x3FFF;
            const uint8_t *gB = &M.gvram[GVRAM_B_OFS], *gR = &M.gvram[GVRAM_R_OFS], *gG = &M.gvram[GVRAM_G_OFS];
            for (int cx = 0; cx < 80; cx++) s += gB[((cx + row_ofs_base) & 0x7FF) + plane_ofs];
            for (int cx = 0; cx < 80; cx++) s += gR[((cx + row_ofs_base) & 0x7FF) + plane_ofs];
            for (int cx = 0; cx < 80; cx++) s += gG[((cx + row_ofs_base) & 0x7FF) + plane_ofs];
        }
        sink += s;
    }
    t_gv = x1Ccount() - c0;

    x1_prof_rd = x1_prof_bmp = x1_prof_txt = 0;
    c0 = x1Ccount();
    for (int y = 0; y < X1_SCREEN_H; y++) x1RenderLine(y);
    t_rd = x1Ccount() - c0;

    Serial.printf("BENCH us/200lines: vpStore=%u intStore=%u vpRMW=%u gvRead=%u render=%u (rd=%u mix=%u) hres=%d\n",
                  (unsigned)(t_vs / 240), (unsigned)(t_is / 240), (unsigned)(t_rmw / 240),
                  (unsigned)(t_gv / 240), (unsigned)(t_rd / 240),
                  (unsigned)(x1_prof_rd / 240), (unsigned)(x1_prof_bmp / 240), (int)hres_mode);
    x1_prof_rd = x1_prof_bmp = x1_prof_txt = 0;
    (void)sink;
}
#endif
