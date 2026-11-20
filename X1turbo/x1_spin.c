/* ============================================================================
 *  X1 / X1turbo: Z80 の待ちループの早送り(X1_IDLE_SKIP。切り替えは z80core_config.h)
 *  2026-11-06 に前の Z80 コアの中に入れたものを、2026-11-20 に z80core のフック(Z80CORE_SPIN)へ移した。
 *
 *  ゲームが割込みや垂直帰線を待つ小さなループ(トップルジップ: IN A,(C) / JP M(BC = 1A01h の垂直表示期間)、
 *  イース: DEC HL / LD A,H / OR L / JR NZ、ザナドゥ: DEC A / JR NZ など)を、回れる周回ぶんまとめて進める。
 *  結果(レジスタ・R・PC・残りサイクル)は 1 命令ずつ実行したときと同じ:
 *  ・JP / JR / DJNZ が 8 バイト以内に戻るとき、ループの中身を 1 回だけ解読する。書き込み・PUSH・CALL・分岐・EI・DAA
 *    などを含まず、I/O は IN r,(C)(BC はループの中で変えない)だけ。ポートは 1 ラインの中で値が変わらず読んでも何も
 *    起きないものだけ(x1_core.cpp x1IoPeek: 8255 の 1A00h〜1A02h・バンク・OPM の状態)
 *  ・種類 1(状態が変わらない待ち): 頭に 2 回続けて戻ったとき、その間がちょうど 1 周ぶんのサイクル(z80core の表から
 *    計算した値。Z80 の通算のサイクルで比べるのでラインをまたいでもよい。割込みの処理や DMA の盗んだぶんが入ると
 *    合わない)で、レジスタが全部同じ、ループが読む I/O の値も同じ、メモリを読むループなら DMA がその間にメモリを
 *    書いていなければ、残りのサイクルで回れる周回をまとめて引く。1 ラインの中で RAM を書くのは Z80 だけ、I/O の値が
 *    変わるのはラインの境目だけなので、以後の周回も同じ
 *  ・種類 2(16 ビットの時間待ち): DEC rr / LD A,上か下 / OR 下か上 / JR NZ か JP NZ
 *  ・種類 3(8 ビットの回数待ち): ... / DEC r / JR NZ か JP NZ、または ... / DJNZ。ほかの命令は数えるレジスタを読み
 *    書きしない。2 回続けて、数えるレジスタがちょうど 1 減り、ほか(DEC のときは F の C 以外を除く)が同じなら、
 *    回数が 1 になるまでと残りサイクルの少ないほうだけ進め、F は最後の DEC のあとと同じにする
 *  ・最後の周回(ループを出る周回)と端数は、ふつうに 1 命令ずつ実行する。割込みが入ったら調べ直す。
 *    4 回続けて条件に合わなかったループは以後調べない
 * ============================================================================ */
#include "z80core.h"
#include "z80core_config.h"
#if defined(ARDUINO)
#include <esp_attr.h>
#else
#define IRAM_ATTR
#endif

unsigned long x1_ext_wr;
int x1_sp_have;
static unsigned sp_clk, sp_base;

int x1Z80Run(z80_t *z, int cycles)
{
#if X1_IDLE_SKIP
  sp_base = sp_clk + (unsigned)cycles;
  const int left = z80core_run(z, cycles);
  sp_clk = sp_base - (unsigned)left;
  return left;
#else
  return z80core_run(z, cycles);
#endif
}

#if X1_IDLE_SKIP
#define SF_ 0x80
#define ZF_ 0x40
#define HF_ 0x10
#define PF_ 0x04
#define NF_ 0x02
#define CF_ 0x01
typedef struct { uint16_t t, b; signed char kind, fails; uint8_t len, cnt, nio, memrd, djnz, rr, m1; int expC; uint8_t code[12]; } SpLoop;
#define SP_N 8
static SpLoop sp_tab[SP_N];
static SpLoop *sp_cur;
static unsigned sp_stamp;
static unsigned long sp_ext;
static uint16_t sp_r[11];
static uint8_t sp_pv;
static inline uint8_t spPeek(uint16_t a) { return x1_rd_map[a >> 12][a & 0x0FFF]; }
enum { RB_B = 1, RB_C = 2, RB_D = 4, RB_E = 8, RB_H = 16, RB_L = 32, RB_A = 128, RB_F = 256, RB_SP = 512, RB_ALT = 1024 };
static const unsigned short spRegBit[8] = { RB_B, RB_C, RB_D, RB_E, RB_H, RB_L, RB_H | RB_L, RB_A };
static const unsigned short spPairBit[4] = { RB_B | RB_C, RB_D | RB_E, RB_H | RB_L, RB_SP };
static int spDecode(uint16_t a, int *c, unsigned *rd, unsigned *wr, int *io, int *mem)
{
  const uint8_t op = spPeek(a);
  uint8_t v;
  *rd = *wr = 0; *io = *mem = 0;
  if (op == 0xCB) {
    v = spPeek((uint16_t)(a + 1));
    if (v < 0x40 || v > 0x7F) return 0;
    *c = z80core_cyc_cb[v]; *rd = spRegBit[v & 7]; *wr = RB_F; *mem = (v & 7) == 6; return 2;
  }
  if (op == 0xED) {
    v = spPeek((uint16_t)(a + 1));
    if ((v & 0xC7) != 0x40 || v == 0x70) return 0;
    *c = z80core_cyc_ed[v]; *rd = RB_B | RB_C | RB_F; *wr = spRegBit[(v >> 3) & 7] | RB_F; *io = 1; return 2;
  }
  *c = z80core_cyc_op[op];
  if (op >= 0x40 && op <= 0x7F) {
    if (op == 0x76 || (op & 0xF8) == 0x70) return 0;
    *rd = spRegBit[op & 7]; *wr = spRegBit[(op >> 3) & 7]; *mem = (op & 7) == 6; return 1;
  }
  if (op >= 0x80 && op <= 0xBF) {
    const int k = (op >> 3) & 7;
    *rd = RB_A | spRegBit[op & 7] | ((k == 1 || k == 3) ? RB_F : 0); *wr = RB_F | (k == 7 ? 0 : RB_A); *mem = (op & 7) == 6; return 1;
  }
  switch (op) {
    case 0x00: return 1;
    case 0x07: case 0x0F: *rd = RB_A | RB_F; *wr = RB_A | RB_F; return 1;
    case 0x17: case 0x1F: *rd = RB_A | RB_F; *wr = RB_A | RB_F; return 1;
    case 0x2F: *rd = RB_A | RB_F; *wr = RB_A | RB_F; return 1;
    case 0x37: *rd = RB_A | RB_F; *wr = RB_F; return 1;
    case 0x3F: *rd = RB_A | RB_F; *wr = RB_F; return 1;
    case 0x0A: *rd = RB_B | RB_C; *wr = RB_A; *mem = 1; return 1;
    case 0x1A: *rd = RB_D | RB_E; *wr = RB_A; *mem = 1; return 1;
    case 0x3A: *wr = RB_A; *mem = 1; return 3;
    case 0x2A: *wr = RB_H | RB_L; *mem = 1; return 3;
    case 0x01: case 0x11: case 0x21: case 0x31: *wr = spPairBit[op >> 4]; return 3;
    case 0x03: case 0x13: case 0x23: case 0x33: case 0x0B: case 0x1B: case 0x2B: case 0x3B:
      *rd = *wr = spPairBit[(op >> 4) & 3]; return 1;
    case 0xEB: *rd = *wr = RB_D | RB_E | RB_H | RB_L; return 1;
    case 0x08: case 0xD9:
      *rd = *wr = RB_ALT | RB_A | RB_F | RB_B | RB_C | RB_D | RB_E | RB_H | RB_L; return 1;
    case 0xC6: case 0xD6: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
      *rd = RB_A; *wr = RB_F | (op == 0xFE ? 0 : RB_A); return 2;
    case 0xCE: case 0xDE: *rd = RB_A | RB_F; *wr = RB_A | RB_F; return 2;
  }
  if ((op & 0xC6) == 0x04 && (op & 0x38) != 0x30) {
    *rd = *wr = spRegBit[(op >> 3) & 7] | RB_F; return 1;
  }
  if ((op & 0xC7) == 0x06 && op != 0x36) { *wr = spRegBit[(op >> 3) & 7]; return 2; }
  return 0;
}
static __attribute__((noinline)) void spClassify(SpLoop *s, uint16_t t, uint16_t b)
{
  uint8_t op;
  uint16_t a = t, last = t;
  int c = 0, blen, bc, n, ci, io, mem, m1 = 1;
  unsigned rd, wr, rdAll = 0, wrAll = 0, rdOther = 0, wrOther = 0, lastRd = 0, lastWr = 0;
  s->t = t; s->b = b; s->kind = 0; s->fails = 0; s->nio = 0; s->memrd = 0; s->djnz = 0;
  op = spPeek(b);
  switch (op) {
    case 0x18: blen = 2; bc = z80core_cyc_op[op]; break;
    case 0x20: case 0x28: case 0x30: case 0x38: blen = 2; bc = z80core_cyc_op[op] + 5; break;
    case 0x10: blen = 2; bc = z80core_cyc_op[op] + 5; s->djnz = 1; break;
    case 0xC3: case 0xC2: case 0xCA: case 0xD2: case 0xDA: case 0xE2: case 0xEA: case 0xF2: case 0xFA:
      blen = 3; bc = z80core_cyc_op[op]; break;
    default: return;
  }
  while (a != b) {
    if ((uint16_t)(b - a) > X1_SP_MAXLEN) return;
    n = spDecode(a, &ci, &rd, &wr, &io, &mem);
    if (!n) return;
    rdOther |= lastRd; wrOther |= lastWr;
    lastRd = rd; lastWr = wr; last = a;
    c += ci; rdAll |= rd; wrAll |= wr;
    m1 += (spPeek(a) == 0xCB || spPeek(a) == 0xED) ? 2 : 1;
    if (mem) s->memrd = 1;
    if (io && ++s->nio > 2) return;
    a = (uint16_t)(a + n);
  }
  if (s->nio && (wrAll & (RB_B | RB_C))) return;
  s->len = (uint8_t)((uint16_t)(b - t) + blen);
  for (int k = 0; k < s->len; k++) s->code[k] = spPeek((uint16_t)(t + k));
  s->expC = c + bc;
  s->m1 = (uint8_t)m1;
  if (s->djnz) {
    if ((rdAll | wrAll) & (RB_B | RB_ALT)) return;
    s->kind = 3; s->cnt = 0; return;
  }
  op = s->code[(uint16_t)(b - t)];
  if (b - t == 3 && (op == 0x20 || op == 0xC2)) {
    const uint8_t d = s->code[0], v = s->code[1], w = s->code[2];
    if ((d == 0x0B && ((v == 0x78 && w == 0xB1) || (v == 0x79 && w == 0xB0))) ||
        (d == 0x1B && ((v == 0x7A && w == 0xB3) || (v == 0x7B && w == 0xB2))) ||
        (d == 0x2B && ((v == 0x7C && w == 0xB5) || (v == 0x7D && w == 0xB4)))) { s->kind = 2; s->rr = (uint8_t)(d >> 4); return; }
  }
  if ((op == 0x20 || op == 0xC2) && b != t) {
    const uint8_t d = spPeek(last);
    if ((d & 0xC7) == 0x05 && d != 0x35) {
      const unsigned rb = spRegBit[(d >> 3) & 7];
      if (!((rdOther | wrOther) & (rb | RB_ALT))) { s->kind = 3; s->cnt = (uint8_t)((d >> 3) & 7); return; }
    }
  }
  s->kind = 1;
}
static inline void spSave(const z80_t *z, uint16_t *r)
{
  r[0] = z->af.w; r[1] = z->bc.w; r[2] = z->de.w; r[3] = z->hl.w; r[4] = z->ix.w; r[5] = z->iy.w; r[6] = z->sp.w;
  r[7] = z->af2; r[8] = z->bc2; r[9] = z->de2; r[10] = z->hl2;
}
static const signed char spCntWord[8] = { 1, 1, 2, 2, 3, 3, -1, 0 };
static inline uint8_t *spCntPtr(z80_t *z, int r)
{
  switch (r) { case 0: return &z->bc.b.h; case 1: return &z->bc.b.l; case 2: return &z->de.b.h; case 3: return &z->de.b.l;
               case 4: return &z->hl.b.h; case 5: return &z->hl.b.l; default: return &z->af.b.h; }
}
static uint8_t spSz53(uint8_t v) { return (uint8_t)((v & 0xA8) | (v ? 0 : ZF_)); }
__attribute__((noinline)) int IRAM_ATTR x1SpinHook(z80_t *z, uint16_t b, uint16_t t, int cyc)
{
  SpLoop *s = &sp_tab[(t ^ (t >> 3)) & (SP_N - 1)];
  if (s != sp_cur || s->t != t || s->b != b) {
    if (s->t != t || s->b != b) spClassify(s, t, b);
    sp_cur = s; x1_sp_have = 0;
  }
  if (s->kind <= 0) return cyc;
  const unsigned now = sp_base - (unsigned)cyc;
  uint8_t pv = 0;
  int pok = 1;
  if (s->nio) pok = x1IoPeek(z->bc.w, &pv);
  if (x1_sp_have && now - sp_stamp == (unsigned)s->expC && cyc > s->expC) {
    int same = pok && (!s->nio || pv == sp_pv) && (!s->memrd || x1_ext_wr == sp_ext);
    for (int k = 0; k < s->len && same; k++)
      if (spPeek((uint16_t)(t + k)) != s->code[k]) { spClassify(s, t, b); x1_sp_have = 0; return cyc; }
    uint16_t r[11];
    spSave(z, r);
    if (s->kind == 1) {
      for (int k = 0; k < 11; k++) if (r[k] != sp_r[k]) same = 0;
      if (same) {
        const int n = (cyc - 1) / s->expC;
        cyc -= n * s->expC; z->r = (uint8_t)(z->r + n * s->m1); s->fails = 0;
      } else if (++s->fails >= 4) { s->kind = 0; return cyc; }
    } else if (s->kind == 2) {
      z80_pair_t *rr = s->rr == 0 ? &z->bc : s->rr == 1 ? &z->de : &z->hl;
      if (same && rr->w == (uint16_t)(sp_r[s->rr + 1] - 1) && rr->w >= 2) {
        int n = (cyc - 1) / s->expC;
        if (n > rr->w - 1) n = rr->w - 1;
        if (n > 0) {
          rr->w = (uint16_t)(rr->w - n);
          z->af.b.h = (uint8_t)(rr->b.h | rr->b.l);
          z->af.b.l = (uint8_t)(spSz53(z->af.b.h) | (__builtin_parity(z->af.b.h) ? 0 : PF_));
          z->r = (uint8_t)(z->r + n * s->m1);
          cyc -= n * s->expC;
        }
      }
    } else {
      const int w = spCntWord[s->cnt];
      const int hi = s->cnt == 7 || !(s->cnt & 1);
      uint8_t *cp = spCntPtr(z, s->cnt);
      const uint8_t prev = (uint8_t)(hi ? (sp_r[w] >> 8) : (sp_r[w] & 0xFF));
      uint16_t m[11];
      for (int k = 0; k < 11; k++) m[k] = 0xFFFF;
      m[w] &= (uint16_t)(hi ? 0x00FF : 0xFF00);
      if (!s->djnz) m[0] &= (uint16_t)(0xFF00 | CF_);
      for (int k = 0; k < 11; k++) if ((r[k] ^ sp_r[k]) & m[k]) same = 0;
      if (same && *cp == (uint8_t)(prev - 1) && *cp >= 2) {
        int n = (cyc - 1) / s->expC;
        if (n > *cp - 1) n = *cp - 1;
        if (n > 0) {
          *cp = (uint8_t)(*cp - n);
          if (!s->djnz) {
            const uint8_t v = *cp;
            z->af.b.l = (uint8_t)(NF_ | (z->af.b.l & CF_) | spSz53(v) | (v == 0x7F ? PF_ : 0) | ((v & 0x0F) == 0x0F ? HF_ : 0));
          }
          z->r = (uint8_t)(z->r + n * s->m1);
          cyc -= n * s->expC; s->fails = 0;
        }
      } else if (++s->fails >= 4) { s->kind = 0; return cyc; }
    }
  }
  spSave(z, sp_r);
  sp_pv = pv; sp_ext = x1_ext_wr;
  sp_stamp = sp_base - (unsigned)cyc; x1_sp_have = 1;
  return cyc;
}
#endif
