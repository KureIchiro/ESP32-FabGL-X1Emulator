// ============================================================================
//  YM2151 (OPM)  ※説明は ym2151.h
//  オペレータ・エンベロープ・フィードバックの計算は PC88/ym2203.cpp と同じ。
//  OPM の決まりごとは MAME の ymfm(opm)に合わせた:
//    音程   : KC の音名 0〜15 は code − (code >> 2) で 0〜12 の半音、KF は 1/64 半音、
//             DT2 は 0 / 384 / 500 / 608(1/64 半音)を足す
//    LFO    : 1 サンプル(チップの FM の周期)ごとに (16 | LFRQ の下位4bit) << 上位4bit を足し、
//             bit22〜29 が LFO の位置。波形 0 = のこぎり、1 = 矩形、2 = 三角、3 = ノイズ
//    AM     : (AM の値 × AMD >> 7) << (AMS − 1) を減衰に足す(AMS 1 で最大 23.9dB)
//    PM     : (PM の値 × PMD >> 7) を PMS 1〜5 は >> (6 − PMS)、6・7 は << (PMS − 5) で 1/64 半音に足す
//    ノイズ : チャンネル8 の C2 を置き換える。17bit LFSR をチップの 1 サンプルに 2 回進め、
//             NFRQ ^ 1Fh 回ごとに取る
// ============================================================================
#include "ym2151.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#ifdef ARDUINO
#include <esp_attr.h>
#define OPM_FAST IRAM_ATTR
#else
#define OPM_FAST
#endif

static uint16_t logSin[256];
static uint16_t expTab[256];
static uint32_t stepTab[768];
static bool     tablesReady;

static void buildTables() {
  if (tablesReady) return;
  for (int i = 0; i < 256; i++) {
    double s = sin((i + 0.5) * M_PI / 512.0);
    logSin[i] = (uint16_t)(-log2(s) * 256.0 + 0.5);
    expTab[i] = (uint16_t)(pow(2.0, -i / 256.0) * 8192.0 + 0.5);
  }
  const double a4 = 440.0 * 64.0 / 3579545.0 * 1048576.0;
  for (int i = 0; i < 768; i++) stepTab[i] = (uint32_t)(a4 * pow(2.0, (i - (4 * 12 + 8) * 64) / 768.0) * 256.0 + 0.5);
  tablesReady = true;
}

static const uint8_t egInc[19 * 8] = {
  0,1,0,1,0,1,0,1,  0,1,0,1,1,1,0,1,  0,1,1,1,0,1,1,1,  0,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,  1,1,1,2,1,1,1,2,  1,2,1,2,1,2,1,2,  1,2,2,2,1,2,2,2,
  2,2,2,2,2,2,2,2,  2,2,2,4,2,2,2,4,  2,4,2,4,2,4,2,4,  2,4,4,4,2,4,4,4,
  4,4,4,4,4,4,4,4,  4,4,4,8,4,4,4,8,  4,8,4,8,4,8,4,8,  4,8,8,8,4,8,8,8,
  8,8,8,8,8,8,8,8,  16,16,16,16,16,16,16,16,  0,0,0,0,0,0,0,0 };
static inline int egSel(int r) {
  if (r < 4) return r == 0 ? 18 : (r & 3);
  if (r < 48) return r & 3;
  if (r < 60) return 4 + (r - 48);
  return 16;
}
static inline int egShift(int r) { return r < 48 ? 11 - (r >> 2) : 0; }

static const uint8_t dtTab[4][32] = {
  { 0 },
  { 0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,8,8 },
  { 1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,16,16,16 },
  { 2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,17,19,20,22,22,22,22 } };
static const uint16_t dt2Tab[4] = { 0, 384, 500, 608 };

enum { EG_ATT, EG_DEC, EG_SUS, EG_REL, EG_OFF };

struct OpmOp {
  uint8_t  dt1, mul, tl, ks, ar, d1r, d2r, d1l, rr, dt2;
  bool     amEn;
  uint32_t phase, inc;
  int16_t  env;
  uint16_t att;
  uint8_t  state, ksr;
};
struct OpmCh {
  OpmOp    op[4];
  uint8_t  kc, kf, kcode;
  uint8_t  fb, con, rl, pms, ams;
  int32_t  fb0, fb1;
  int16_t  pmLast;
};
struct Ym2151 {
  uint32_t clock, rate;
  uint64_t scaleQ16;
  uint32_t egAcc, egCnt;
  OpmCh    ch[8];
  uint8_t  lfrq, amd, pmd, wave;
  bool     lfoReset;
  uint64_t lfoAcc;
  int32_t  lfoAm, lfoPm;
  bool     noiseOn; uint8_t nfrq;
  uint32_t noiseAcc, noiseCnt, lfsr, noiseState;
  uint32_t fbIncLo, fbIncHi;
};

static inline int OPM_FAST opCalc(OpmOp &o, int mod, int att) {
  uint32_t idx = ((o.phase >> 22) + (uint32_t)mod) & 1023;
  uint32_t j = idx & 255;
  if (idx & 256) j = 255 - j;
  uint32_t x = logSin[j] + ((uint32_t)att << 2);
  int v = (x >> 8) >= 14 ? 0 : (expTab[x & 255] >> (x >> 8));
  return (idx & 512) ? -v : v;
}

static void updateInc(Ym2151 *opm, OpmCh &ch, int k, int pm) {
  OpmOp &o = ch.op[k];
  int code = ch.kc & 15, oct = (ch.kc >> 4) & 7;
  int idx = oct * 768 + (code - (code >> 2)) * 64 + (ch.kf >> 2) + dt2Tab[o.dt2] + pm;
  if (idx < 0) idx = 0;
  uint32_t step = (stepTab[idx % 768] << (idx / 768)) >> 8;
  int d = dtTab[o.dt1 & 3][ch.kcode];
  int32_t fc = (int32_t)step + ((o.dt1 & 4) ? -d : d);
  if (fc > 0x1FFFF) fc = 0x1FFFF; else if (fc < 0) fc &= 0x1FFFF;
  uint32_t inc20 = o.mul ? (uint32_t)fc * o.mul : (uint32_t)fc >> 1;
  o.inc = (uint32_t)(((uint64_t)inc20 * opm->scaleQ16) >> 4);
  o.ksr = (uint8_t)(ch.kcode >> (3 - o.ks));
}
static void updateCh(Ym2151 *opm, int c) {
  OpmCh &ch = opm->ch[c];
  ch.kcode = (uint8_t)((ch.kc >> 2) & 31);
  int pm = ch.pms ? ch.pmLast : 0;
  for (int k = 0; k < 4; k++) updateInc(opm, ch, k, pm);
}

static inline int rateFor(const OpmOp &o, int base) {
  if (!base) return 0;
  int r = base + o.ksr;
  return r > 63 ? 63 : r;
}
static inline void updateAtt(OpmOp &o) {
  int a = o.env + (o.tl << 3);
  o.att = (uint16_t)(a > 1023 ? 1023 : a);
}
static void keyOn(OpmOp &o) {
  if (o.state != EG_OFF && o.state != EG_REL) return;
  o.phase = 0;
  o.state = EG_ATT;
  if (rateFor(o, o.ar * 2) >= 62) { o.env = 0; o.state = EG_DEC; }
  updateAtt(o);
}
static void keyOff(OpmOp &o) { if (o.state != EG_OFF) o.state = EG_REL; }

static void OPM_FAST egStep(Ym2151 *opm, OpmOp &o) {
  int r;
  switch (o.state) {
    case EG_ATT: r = rateFor(o, o.ar * 2); break;
    case EG_DEC: r = rateFor(o, o.d1r * 2); break;
    case EG_SUS: r = rateFor(o, o.d2r * 2); break;
    case EG_REL: r = rateFor(o, o.rr * 4 + 2); break;
    default: return;
  }
  int sh = egShift(r);
  if (opm->egCnt & ((1u << sh) - 1)) return;
  int inc = egInc[egSel(r) * 8 + ((opm->egCnt >> sh) & 7)];
  if (!inc) return;
  switch (o.state) {
    case EG_ATT:
      o.env += (~o.env * inc) >> 4;
      if (o.env <= 0) { o.env = 0; o.state = EG_DEC; }
      break;
    case EG_DEC: {
      o.env += inc;
      int sl = o.d1l == 15 ? 31 * 32 : o.d1l * 32;
      if (o.env >= sl) { o.env = (int16_t)sl; o.state = EG_SUS; }
      break;
    }
    case EG_SUS:
      o.env += inc;
      if (o.env >= 1023) o.env = 1023;
      break;
    case EG_REL:
      o.env += inc;
      if (o.env >= 1023) { o.env = 1023; o.state = EG_OFF; }
      break;
  }
  updateAtt(o);
}

Ym2151 *ym2151Create(uint32_t clock, uint32_t rate) {
  buildTables();
  Ym2151 *opm = (Ym2151 *)calloc(1, sizeof(Ym2151));
  if (!opm) return nullptr;
  opm->clock = clock;
  opm->rate = rate;
  ym2151Reset(opm);
  return opm;
}

void ym2151Reset(Ym2151 *opm) {
  uint32_t clock = opm->clock, rate = opm->rate;
  memset(opm, 0, sizeof(*opm));
  opm->clock = clock; opm->rate = rate;
  opm->scaleQ16 = (uint64_t)((double)clock / 64.0 / rate * 65536.0 + 0.5);
  if (OPM_FB_CLIP_ENABLE && rate < OPM_FB_CLIP_RATE) {
    double lo = (double)OPM_FB_CLIP_LO_HZ / rate, hi = (double)OPM_FB_CLIP_HI_HZ / rate;
    opm->fbIncLo = lo >= 1.0 ? 0xFFFFFFFFu : (uint32_t)(lo * 4294967296.0);
    opm->fbIncHi = hi >= 1.0 ? 0xFFFFFFFFu : (uint32_t)(hi * 4294967296.0);
  }
  opm->lfsr = 1;
  for (int c = 0; c < 8; c++) {
    opm->ch[c].rl = 3;
    for (int k = 0; k < 4; k++) {
      OpmOp &o = opm->ch[c].op[k];
      o.state = EG_OFF; o.env = 1023; o.tl = 127; o.mul = 1;
      updateAtt(o);
    }
    updateCh(opm, c);
  }
}

void ym2151WriteReg(Ym2151 *opm, uint8_t reg, uint8_t v) {
  if (reg < 0x20) {
    switch (reg) {
      case 0x01: opm->lfoReset = (v & 0x02) != 0; if (opm->lfoReset) opm->lfoAcc = 0; break;
      case 0x08: {
        OpmCh &ch = opm->ch[v & 7];
        for (int k = 0; k < 4; k++) { if (v & (0x08 << k)) keyOn(ch.op[k]); else keyOff(ch.op[k]); }
        break;
      }
      case 0x0F: opm->noiseOn = (v & 0x80) != 0; opm->nfrq = v & 0x1F; break;
      case 0x18: opm->lfrq = v; break;
      case 0x19: if (v & 0x80) opm->pmd = v & 0x7F; else opm->amd = v & 0x7F; break;
      case 0x1B: opm->wave = v & 3; break;
      default: break;
    }
    return;
  }
  int c = reg & 7;
  OpmCh &ch = opm->ch[c];
  if (reg < 0x40) {
    switch (reg & 0xF8) {
      case 0x20: ch.rl = v >> 6; ch.fb = (v >> 3) & 7; ch.con = v & 7; break;
      case 0x28: ch.kc = v & 0x7F; updateCh(opm, c); break;
      case 0x30: ch.kf = v & 0xFC; updateCh(opm, c); break;
      case 0x38: ch.pms = (v >> 4) & 7; ch.ams = v & 3; updateCh(opm, c); break;
    }
    return;
  }
  static const uint8_t slotOfReg[4] = { 0, 2, 1, 3 };
  OpmOp &o = ch.op[slotOfReg[(reg >> 3) & 3]];
  switch (reg & 0xE0) {
    case 0x40: o.dt1 = (v >> 4) & 7; o.mul = v & 15; updateCh(opm, c); break;
    case 0x60: o.tl = v & 0x7F; updateAtt(o); break;
    case 0x80: o.ks = v >> 6; o.ar = v & 0x1F; updateCh(opm, c); break;
    case 0xA0: o.amEn = (v & 0x80) != 0; o.d1r = v & 0x1F; break;
    case 0xC0: o.dt2 = v >> 6; o.d2r = v & 0x1F; updateCh(opm, c); break;
    case 0xE0: o.d1l = v >> 4; o.rr = v & 15; break;
  }
}

static inline void lfsrStep(Ym2151 *opm) {
  uint32_t l = opm->lfsr;
  l = (l << 1) | (((l >> 17) ^ (l >> 14) ^ 1) & 1);
  opm->lfsr = l & 0x3FFFF;
}

void ym2151CsmKeyOn(Ym2151 *opm) {
  for (int c = 0; c < 8; c++)
    for (int k = 0; k < 4; k++) { keyOff(opm->ch[c].op[k]); keyOn(opm->ch[c].op[k]); }
}

void OPM_FAST ym2151Generate(Ym2151 *opm, int32_t *out, int n) {
  const uint32_t egStepQ16 = (uint32_t)opm->scaleQ16, egPeriod = 3u << 16;
  const bool useNoise = opm->noiseOn || opm->wave == 3;
  for (int s = 0; s < n; s++) {
    if (useNoise) {
      opm->noiseAcc += egStepQ16 * 2;
      uint32_t freq = opm->nfrq ^ 0x1F;
      while (opm->noiseAcc >= 0x10000) {
        opm->noiseAcc -= 0x10000;
        lfsrStep(opm);
        if (opm->noiseCnt++ >= freq) { opm->noiseCnt = 0; opm->noiseState = (opm->lfsr >> 17) & 1; }
      }
    }
    if (!opm->lfoReset) opm->lfoAcc += (uint64_t)((0x10 | (opm->lfrq & 15)) << (opm->lfrq >> 4)) * opm->scaleQ16;
    uint32_t lfo = (uint32_t)(opm->lfoAcc >> (16 + 22)) & 255;
    int32_t am, pm;
    switch (opm->wave) {
      case 0: am = lfo ^ 0xFF; pm = (int8_t)lfo; break;
      case 1: am = (lfo & 0x80) ? 0 : 0xFF; pm = (int8_t)(am ^ 0x80); break;
      case 2: am = ((lfo & 0x80) ? (lfo << 1) : ((lfo ^ 0xFF) << 1)) & 0xFF;
              pm = (int8_t)((lfo & 0x40) ? am : ~am); break;
      default: am = (opm->lfsr >> 2) & 0xFF; pm = (int8_t)am; break;
    }
    opm->lfoAm = (am * opm->amd) >> 7;
    opm->lfoPm = (pm * opm->pmd) >> 7;

    opm->egAcc += egStepQ16;
    while (opm->egAcc >= egPeriod) {
      opm->egAcc -= egPeriod;
      opm->egCnt++;
      for (int c = 0; c < 8; c++)
        for (int k = 0; k < 4; k++) egStep(opm, opm->ch[c].op[k]);
    }

    int32_t sum = 0;
    for (int c = 0; c < 8; c++) {
      OpmCh &ch = opm->ch[c];
      OpmOp *op = ch.op;
      if (op[0].state == EG_OFF && op[1].state == EG_OFF && op[2].state == EG_OFF && op[3].state == EG_OFF) continue;
      if (ch.pms) {
        int p = opm->lfoPm;
        p = ch.pms < 6 ? (p >> (6 - ch.pms)) : (p << (ch.pms - 5));
        if (p != ch.pmLast) { ch.pmLast = (int16_t)p; for (int k = 0; k < 4; k++) updateInc(opm, ch, k, p); }
      }
      int amOfs = ch.ams ? (opm->lfoAm << (ch.ams - 1)) : 0;
      int a0 = op[0].att, a1 = op[1].att, a2 = op[2].att, a3 = op[3].att;
      if (amOfs) {
        if (op[0].amEn) { a0 += amOfs; if (a0 > 1023) a0 = 1023; }
        if (op[1].amEn) { a1 += amOfs; if (a1 > 1023) a1 = 1023; }
        if (op[2].amEn) { a2 += amOfs; if (a2 > 1023) a2 = 1023; }
        if (op[3].amEn) { a3 += amOfs; if (a3 > 1023) a3 = 1023; }
      }
      int fbm = 0;
      if (ch.fb) {
        fbm = (int)((ch.fb0 + ch.fb1) >> (10 - ch.fb));
        uint32_t inc0 = op[0].inc;
        if (inc0 < opm->fbIncHi) {
          int e = ch.fb * 64 - a0;
          int emax = inc0 < opm->fbIncLo ? OPM_FB_CLIP_MAX_LO : OPM_FB_CLIP_MAX;
          if (e > emax && e < OPM_FB_CLIP_NOISE) {
            int d = (e - emax) * 4;
            fbm = (d >> 8) >= 14 ? 0 : (fbm * (int)(expTab[d & 255] >> (d >> 8))) >> 13;
          }
        }
      }
      int o1 = opCalc(op[0], fbm, a0), o2, o3, o4, v;
      ch.fb0 = ch.fb1; ch.fb1 = o1;
      bool noise4 = (c == 7) && opm->noiseOn;
      #define OP4(mod) (noise4 ? noiseOut(a3) : opCalc(op[3], (mod), a3))
      auto noiseOut = [&](int att) -> int {
        uint32_t x = (uint32_t)att << 2;
        int lv = (x >> 8) >= 14 ? 0 : (expTab[x & 255] >> (x >> 8));
        return opm->noiseState ? lv : -lv;
      };
      switch (ch.con) {
        case 0: o2 = opCalc(op[1], o1 >> 1, a1); o3 = opCalc(op[2], o2 >> 1, a2); v = OP4(o3 >> 1); break;
        case 1: o2 = opCalc(op[1], 0, a1); o3 = opCalc(op[2], (o1 + o2) >> 1, a2); v = OP4(o3 >> 1); break;
        case 2: o2 = opCalc(op[1], 0, a1); o3 = opCalc(op[2], o2 >> 1, a2); v = OP4((o1 + o3) >> 1); break;
        case 3: o2 = opCalc(op[1], o1 >> 1, a1); o3 = opCalc(op[2], 0, a2); v = OP4((o2 + o3) >> 1); break;
        case 4: o2 = opCalc(op[1], o1 >> 1, a1); o3 = opCalc(op[2], 0, a2); o4 = OP4(o3 >> 1); v = o2 + o4; break;
        case 5: o2 = opCalc(op[1], o1 >> 1, a1); o3 = opCalc(op[2], o1 >> 1, a2); o4 = OP4(o1 >> 1); v = o2 + o3 + o4; break;
        case 6: o2 = opCalc(op[1], o1 >> 1, a1); o3 = opCalc(op[2], 0, a2); o4 = OP4(0); v = o2 + o3 + o4; break;
        default: o2 = opCalc(op[1], 0, a1); o3 = opCalc(op[2], 0, a2); o4 = OP4(0); v = o1 + o2 + o3 + o4; break;
      }
      #undef OP4
      for (int k = 0; k < 4; k++) op[k].phase += op[k].inc;
      if (!ch.rl) continue;
      if (v > 8191) v = 8191; else if (v < -8192) v = -8192;
      sum += v;
    }
    out[s] = sum;
  }
}

static inline int32_t periodA(const Ym2151Timer &t) { return (int32_t)(1024 - t.na) * 64; }
static inline int32_t periodB(const Ym2151Timer &t) { return (int32_t)(256 - t.nb) * 1024; }

void ym2151TimerReset(Ym2151Timer &t) { memset(&t, 0, sizeof(t)); }

void ym2151TimerWrite(Ym2151Timer &t, uint8_t reg, uint8_t v) {
  switch (reg) {
    case 0x10: t.na = (uint16_t)((t.na & 3) | (v << 2)); break;
    case 0x11: t.na = (uint16_t)((t.na & 0x3FC) | (v & 3)); break;
    case 0x12: t.nb = v; break;
    case 0x14: {
      uint8_t old = t.ctrl;
      if ((v & 1) && !(old & 1)) t.cntA = periodA(t);
      if ((v & 2) && !(old & 2)) t.cntB = periodB(t);
      if (v & 0x10) t.status &= ~1;
      if (v & 0x20) t.status &= ~2;
      t.ctrl = v;
      break;
    }
    default: break;
  }
}

void ym2151TimerAdvance(Ym2151Timer &t, uint32_t clocks, void (*csm)(uint32_t at)) {
  if (t.ctrl & 1) {
    int32_t r = t.cntA;
    while (r <= (int32_t)clocks) {
      if (t.ctrl & 4) t.status |= 1;
      if ((t.ctrl & 0x80) && csm) csm(r > 0 ? (uint32_t)r : 0);
      r += periodA(t);
    }
    t.cntA = r - (int32_t)clocks;
  }
  if (t.ctrl & 2) {
    t.cntB -= (int32_t)clocks;
    while (t.cntB <= 0) { t.cntB += periodB(t); if (t.ctrl & 8) t.status |= 2; }
  }
}
