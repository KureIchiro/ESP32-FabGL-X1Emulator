// ============================================================================
//  X1turbo machine core: メモリバンク切替 / I/Oディスパッチ / CRTC / パレット / PCG / 漢字ROM
//  カセットテープには対応しない（ユーザー指示によりスコープ外）。
//  turbo の部分は Common Source Project(vm/x1 の display.cpp / iobus.cpp / memory.cpp)と同じ。
// ============================================================================
// 【最適化】Arduinoのesp32コアは既定で -Os。メモリ/IOバス はフレームごとに
// 数万回まわるため、この翻訳単位だけ O3 に引き上げる。
// (X1turbo.ino:20 の #pragma は .ino の翻訳単位にしか効かない)
#pragma GCC optimize("O3")

#include "x1.h"
#include "ym2151.h"

#if X1_OPM_BOARD
static Ym2151Timer opmTimer;
static uint8_t opmAddr;
#endif
#include <string.h>

X1Memory M;

uint8_t x1_mode1 = 0;
uint8_t x1_mode1_eff = 0;
uint8_t x1_mode2 = 0;
static uint8_t bank_reg = 0x10;

static bool rom_selected;

uint8_t *x1_rd_map[16];
uint8_t *x1_wr_map[16];

static inline uint8_t *ramBlock(int i) {
    return M.ram_page[i >> 2] + (i & 3) * 0x1000;
}

static void updateMap() {
    for (int i = 0; i < 8; i++) {
        if (!(bank_reg & 0x10)) {
            uint8_t *p = M.extram + (bank_reg & 0x0F) * 0x8000 + i * 0x1000;
            x1_rd_map[i] = x1_wr_map[i] = p;
        } else {
            x1_wr_map[i] = ramBlock(i);
            x1_rd_map[i] = rom_selected ? M.ipl_rom + i * 0x1000 : ramBlock(i);
        }
    }
}
static void setRomSelected(bool sel) {
    rom_selected = sel;
    updateMap();
}

bool x1RomSelected() { return rom_selected; }

static void initMemMap() {
    for (int i = 0; i < 16; i++) x1_rd_map[i] = x1_wr_map[i] = ramBlock(i);
    updateMap();
}

uint8_t crtc_regs[18];
static uint8_t crtc_index;

static inline uint16_t crtc_start_addr() {
    return ((crtc_regs[12] << 8) | crtc_regs[13]) & 0x07FF;
}

int  x1_cyc_line    = X1_CYCLES_PER_LINE;
int  x1_lines_frame = X1_LINES_PER_FRAME;
int  x1_disp_lines  = 200;
bool x1_hireso      = false;

void x1FrameStart() {
    const int ch = (crtc_regs[9] & 0x1F) + 1;
    int vt = ((crtc_regs[4] & 0x7F) + 1) * ch + (crtc_regs[5] & 0x1F);
    if (vt < 200 || vt > X1_MAX_LINES) vt = X1_LINES_PER_FRAME;
    x1_hireso = vt >= 400;
    x1_mode1_eff = x1_hireso ? x1_mode1 : (uint8_t)(x1_mode1 & 0xF8);
    int cyc = X1_CYCLES_PER_LINE;
    if (crtc_regs[0] >= 20) {
        const uint64_t num = (uint64_t)(crtc_regs[0] + 1) * (hres_mode ? 16 : 32) * 4000000u * 1000u;
        const uint64_t den = (x1_mode1_eff & 1) ? 42954545u : 28636364u;
        cyc = (int)((num / den + 500) / 1000);
        if (cyc < 100) cyc = 100;
        if (cyc > 400) cyc = 400;
    }
    x1_cyc_line = cyc;
    x1_lines_frame = vt;
    x1_disp_lines = x1_hireso ? 400 : 200;
    x1VideoFrameStart();
}

uint8_t pal_b, pal_r, pal_g;

uint8_t priority_reg;

static bool gvram_ext_mode;
static bool prev_portc_bit5;
bool hres_mode;
static uint8_t *gvAccess;
static int gvAccessPage;

void x1SetPortCBits(uint8_t bit5, uint8_t bit6) {
    if (prev_portc_bit5 && !bit5) {
        gvram_ext_mode = true;
    }
    prev_portc_bit5 = bit5;
    hres_mode = (bit6 == 0);
}

static uint16_t kaddr;
static uint32_t kanjiOfs;
static int kofs, kflag;

static uint16_t jis2sjis(uint16_t jis) {
    if (!jis) return 0;
    int c1 = jis >> 8, c2 = jis & 0xFF;
    if (c1 & 1) { c2 += 0x1F; if (c2 >= 0x7F) c2++; }
    else c2 += 0x7E;
    c1 = (c1 - 0x21) / 2 + 0x81;
    if (c1 >= 0xA0) c1 += 0x40;
    return (uint16_t)((c1 << 8) | c2);
}
static uint32_t jis2knj(uint16_t jis) {
    uint32_t sjis = jis2sjis(jis);
    if (sjis < 0x100) return sjis * 16;
    if (sjis >= 0x8140 && sjis < 0x84C0) return 0x01000 + (sjis - 0x8140) * 32;
    if (sjis >= 0x8890 && sjis < 0xA000) return 0x08000 + (sjis - 0x8890) * 32;
    if (sjis >= 0xE040 && sjis < 0xEAB0) return 0x36E00 + (sjis - 0xE040) * 32;
    return 0;
}
static uint16_t jis2adr_x1(uint16_t jis) {
    int jh = jis >> 8, jl = jis & 0xFF;
    int adr = jh > 0x28 ? 0x4000 + (jh - 0x30) * 0x600 : 0x0100 + (jh - 0x21) * 0x600;
    if (jl >= 0x20) adr += (jl - 0x20) * 0x10;
    return (uint16_t)adr;
}
static uint32_t adr2knj_x1(int adr) {
    int jh = adr < 0x4000 ? 0x21 + (adr - 0x100) / 0x600 : 0x30 + (adr - 0x4000) / 0x600;
    if (jh > 0x28) adr -= 0x4000 + (jh - 0x30) * 0x600;
    else adr -= 0x100 + (jh - 0x21) * 0x600;
    int jl = 0x20;
    if (adr) jl += adr / 0x10;
    return jis2knj((uint16_t)((jh << 8) | jl));
}
uint32_t x1Adr2KnjX1t(uint16_t adr) {
    int rh = (adr >> 8) & 0x1F, rl = adr & 0xFF;
    if (rl == 0 && rh == 0) return 0;
    int j2 = rl & 0x1F, j1 = (rl / 0x20) & 7;
    if (rh < 4 || rh > 0x1C) {
        j1 |= rh < 4 ? 0x20 : 0x70;
        switch (rh & 3) {
            case 0: j2 |= 0x20; break;
            case 1: j2 |= 0x60; break;
            case 2: j2 |= 0x40; break;
            default: j1 = j2 = 0; break;
        }
    } else {
        j1 |= (((rh >> 1) + 7) / 3) * 0x10;
        j1 |= (rh & 1) * 8;
        j2 |= ((((rh >> 1) + 1) % 3) + 1) * 0x20;
    }
    return jis2knj((uint16_t)((j1 << 8) | j2));
}

void x1KanjiDeinterleave(uint8_t *kanji) {
    uint8_t buf[32];
    for (uint32_t ofs = 0x1000; ofs + 32 <= X1_KANJI_SIZE; ofs += 32) {
        for (int i = 0; i < 16; i++) { buf[i] = kanji[ofs + i * 2]; buf[i + 16] = kanji[ofs + i * 2 + 1]; }
        memcpy(kanji + ofs, buf, 32);
    }
}

static uint8_t kanjiRead(uint16_t port) {
    if (port == 0x0E80 || port == 0x0E81) {
        if (!(kaddr & 0xFF00)) return port == 0x0E80 ? (uint8_t)(jis2adr_x1((uint16_t)(kaddr << 8)) >> 8) : 0;
        uint8_t v = M.kanji[(kanjiOfs + kofs + (port == 0x0E81 ? 16 : 0)) % X1_KANJI_SIZE];
        kflag |= port == 0x0E80 ? 1 : 2;
        if (kflag == 3) { kofs = (kofs + 1) & 15; kflag = 0; }
        return v;
    }
    return 0xFF;
}
static void kanjiWrite(uint16_t port, uint8_t val) {
    switch (port) {
        case 0x0E80: kaddr = (kaddr & 0xFF00) | val; break;
        case 0x0E81: kaddr = (kaddr & 0x00FF) | (val << 8); break;
        case 0x0E82: kanjiOfs = adr2knj_x1(kaddr & 0xFFF0); break;
        default: break;
    }
}

void x1Init(const X1Memory &mem) {
    M = mem;
    x1Reset();
}

#if X1_OPM_BOARD
static void opmReset() { ym2151TimerReset(opmTimer); opmAddr = 0; }
#endif

static void setGvAccess() {
    gvAccessPage = (x1_mode1 & 0x10) ? 1 : 0;
    gvAccess = M.gvram + gvAccessPage * GVRAM_PAGE_SZ;
}

void x1IrqReset();
void x1Reset() {
    x1IrqReset();
    rom_selected = true;
    bank_reg = 0x10;
    initMemMap();
    memset(crtc_regs, 0, sizeof(crtc_regs));
    crtc_index = 0;
    crtc_regs[1] = 40;
    crtc_regs[6] = 25;
    crtc_regs[9] = 7;

    x1_mode1 = 0;
    x1_mode2 = 0;
    setGvAccess();
    pal_b = pal_r = pal_g = 0;
    x1VideoPaletteChanged();

    priority_reg = 0;
    x1VideoUpdatePriority();
    gvram_ext_mode = false;
    prev_portc_bit5 = true;
    hres_mode = false;

    memset(M.tvram, 0, 0x800);
    memset(M.avram, 0, 0x800);
    memset(M.kvram, 0, 0x800);
    for (int i = 0; i < 3; i++) memset(M.pcg[i], 0, 0x800);
    memset(M.gvram, 0, GVRAM_ALLOC_SZ);
    kaddr = 0; kanjiOfs = 0; kofs = 0; kflag = 0;
    x1VideoInvalidate();
#if X1_OPM_BOARD
    opmReset();
#endif
    x1SioReset();
    x1DmaReset();
}

extern "C" uint8_t IRAM_ATTR x1ReadByte(uint16_t addr) {
    return x1_rd_map[addr >> 12][addr & 0x0FFF];
}

extern "C" void IRAM_ATTR x1WriteByte(uint16_t addr, uint8_t val) {
    x1_wr_map[addr >> 12][addr & 0x0FFF] = val;
}

static inline int pcgVpos() {
    int vpos = x1_current_scanline + 1;
    return vpos >= x1_lines_frame ? 0 : vpos;
}
static void pcgBeamPos(int &vpos, int &hbeam) {
    vpos = pcgVpos();
    int cyc = x1LineCycle(), len = x1_cyc_line;
    if (x1_hireso && (crtc_regs[9] & 0x1F) >= 8 && vpos >= x1_disp_lines) {
        const int t = (vpos - x1_disp_lines) * x1_cyc_line + cyc;
        vpos = x1_disp_lines + t / X1_CYCLES_PER_LINE * 2;
        cyc = t % X1_CYCLES_PER_LINE;
        len = X1_CYCLES_PER_LINE;
    }
    hbeam = cyc * (crtc_regs[0] + 1) / len;
}
static uint16_t pcgBeamAddr(int rowLines, int vpos, int hbeam) {
    int vbeam = vpos / rowLines;
    return (uint16_t)((hbeam + vbeam * crtc_regs[1] + crtc_start_addr()) & 0x7FF);
}
static inline int pcgBeamLine(int rowLines, int ysize, int vpos) {
    int l = vpos % rowLines;
    return (rowLines > 8 && x1_hireso ? l >> 1 : l) & (ysize - 1);
}
static uint16_t fastAddr(bool pcg) {
    static const uint16_t cand[4] = { 0x7FF, 0x3FF, 0x5FF, 0x1FF };
    for (int i = 0; i < 4; i++)
        if (((M.avram[cand[i]] & 0x20) != 0) == pcg) return cand[i];
    return 0x3FF;
}
static inline int fastPcgOfs(uint16_t off) {
    uint16_t v = fastAddr(true);
    int code = M.tvram[v];
    if (M.kvram[v] & 0x90) code = (code & 0xFE) + (off & 1);
    return (code * 8 + ((off >> 1) & 7)) & 0x7FF;
}
static uint8_t pcgRead(uint16_t off) {
    int plane = (off >> 8) & 0x03;
    if (x1_mode1 & 0x20) {
        if (plane) return M.pcg[plane - 1][fastPcgOfs(off)];
        uint16_t v = fastAddr(false);
        uint8_t ank = M.tvram[v], knj = M.kvram[v];
        if (knj & 0x80) {
            uint32_t o = x1Adr2KnjX1t((uint16_t)((knj << 8) | ank)) + ((knj & 0x40) ? 16 : 0);
            return M.kanji[(o | (off & 15)) % X1_KANJI_SIZE];
        }
        if (x1_mode1 & 0x40) return M.kanji[(ank << 4) | (off & 15)];
        return M.cgrom[(ank << 3) | ((off >> 1) & 7)];
    }
    int ch = (crtc_regs[9] & 0x1F) + 1;
    int ysize = ch > 8 ? 8 : ch;
    int vpos, hbeam;
    pcgBeamPos(vpos, hbeam);
    int ofs = (M.tvram[pcgBeamAddr(ch, vpos, hbeam)] * 8 + pcgBeamLine(ch, ysize, vpos)) & 0x7FF;
    return plane == 0 ? M.cgrom[ofs] : M.pcg[plane - 1][ofs];
}
static void pcgWrite(uint16_t off, uint8_t val) {
    int plane = (off >> 8) & 0x03;
    if (plane == 0) return;
    if (x1_mode1 & 0x20) {
        M.pcg[plane - 1][fastPcgOfs(off)] = val;
        x1_pcg_gen++;
        return;
    }
    int ch = (crtc_regs[9] & 0x1F) + 1;
    int ofs, vpos, hbeam;
    pcgBeamPos(vpos, hbeam);
    if (x1_hireso) {
        ofs = (M.tvram[pcgBeamAddr(ch, vpos, hbeam)] * 8 + pcgBeamLine(ch, 8, vpos)) & 0x7FF;
    } else {
        int ysize = ch > 8 ? ch - 8 : ch;
        if (ysize < 1) ysize = 1;
        ofs = (M.tvram[pcgBeamAddr(ysize, vpos, hbeam)] * 8 + (vpos & (ysize - 1))) & 0x7FF;
    }
    M.pcg[plane - 1][ofs] = val;
    x1_pcg_gen++;
}

static uint8_t gvramRead(uint16_t addr) {
    switch (addr & 0xC000) {
        case 0x4000: return gvAccess[GVRAM_B_OFS + (addr & 0x3FFF)];
        case 0x8000: return gvAccess[GVRAM_R_OFS + (addr & 0x3FFF)];
        case 0xC000: return gvAccess[GVRAM_G_OFS + (addr & 0x3FFF)];
        default:     return 0xFF;
    }
}

static void gvramWrite(uint16_t addr, uint8_t val) {
    uint16_t ofs = addr & 0x3FFF;
    uint8_t *g = gvAccess;
    x1MarkGv(gvAccessPage, ofs);
    if (!gvram_ext_mode) {
        switch (addr & 0xC000) {
            case 0x4000: g[GVRAM_B_OFS + ofs] = val; break;
            case 0x8000: g[GVRAM_R_OFS + ofs] = val; break;
            case 0xC000: g[GVRAM_G_OFS + ofs] = val; break;
            default: break;
        }
    } else {
        switch (addr & 0xC000) {
            case 0x0000:
                g[GVRAM_B_OFS + ofs] = val;
                g[GVRAM_R_OFS + ofs] = val;
                g[GVRAM_G_OFS + ofs] = val;
                break;
            case 0x4000:
                g[GVRAM_R_OFS + ofs] = val;
                g[GVRAM_G_OFS + ofs] = val;
                break;
            case 0x8000:
                g[GVRAM_B_OFS + ofs] = val;
                g[GVRAM_G_OFS + ofs] = val;
                break;
            case 0xC000:
                g[GVRAM_B_OFS + ofs] = val;
                g[GVRAM_R_OFS + ofs] = val;
                break;
        }
    }
}

extern "C" uint8_t IRAM_ATTR x1ReadIO(uint16_t port) {
    gvram_ext_mode = false;
    if (port >= 0x4000) {
        return gvramRead(port);
    }
    switch (port & 0xFF00) {
#if X1_OPM_BOARD
        case 0x0700:
            if (port <= 0x0701) return opmTimer.status;
            if (port <= 0x0707) return x1CtcRead(port);
            return 0xFF;
#endif
        case 0x0B00:
            return bank_reg;
        case 0x0E00:
            return kanjiRead(port);
        case 0x0F00:
            if (port >= 0x0FF8 && port <= 0x0FFF) return x1FdcReadIO(port);
            return 0xFF;
        case 0x1400: case 0x1500: case 0x1600: case 0x1700:
            return pcgRead(port & 0x3FF);
        case 0x1800:
            return 0xFF;
        case 0x1900:
            return x1SubIoRead();
        case 0x1B00:
            return x1PsgRead();
        case 0x1A00:
            return x1PpiRead(port);
        case 0x1E00:
            setRomSelected(false);
            return 0xFF;
        case 0x1F00:
            if (port >= 0x1F80 && port <= 0x1F8F) return x1DmaRead();
            if ((port >= 0x1F90 && port <= 0x1F93) || (port >= 0x1F98 && port <= 0x1F9B)) return x1SioRead(port);
            if (port >= 0x1FA0 && port <= 0x1FAB) return x1CtcRead(port);
            if (port == 0x1FF0) return 0x00;
            return 0xFF;
        case 0x2000: case 0x2100: case 0x2200: case 0x2300:
        case 0x2400: case 0x2500: case 0x2600: case 0x2700:
        case 0x2800: case 0x2900: case 0x2A00: case 0x2B00:
        case 0x2C00: case 0x2D00: case 0x2E00: case 0x2F00:
            return M.avram[port & 0x7FF];
        case 0x3000: case 0x3100: case 0x3200: case 0x3300:
        case 0x3400: case 0x3500: case 0x3600: case 0x3700:
            return M.tvram[port & 0x7FF];
        case 0x3800: case 0x3900: case 0x3A00: case 0x3B00:
        case 0x3C00: case 0x3D00: case 0x3E00: case 0x3F00:
            return M.kvram[port & 0x7FF];
        default:
            return 0xFF;
    }
}

extern "C" int IRAM_ATTR x1IoPeek(uint16_t port, uint8_t *v) {
    switch (port & 0xFF00) {
#if X1_OPM_BOARD
        case 0x0700: if (port <= 0x0701) { *v = opmTimer.status; return 1; } return 0;
#endif
        case 0x0B00: *v = bank_reg; return 1;
        case 0x1A00: if ((port & 3) != 3) { *v = x1PpiRead(port); return 1; } return 0;
    }
    return 0;
}

extern "C" void IRAM_ATTR x1WriteIO(uint16_t port, uint8_t val) {
    if (port >= 0x4000 || gvram_ext_mode) {
        gvramWrite(port, val);
        return;
    }
    switch (port & 0xFF00) {
#if X1_OPM_BOARD
        case 0x0700:
            if (port == 0x0700) opmAddr = val;
            else if (port == 0x0701) {
            ym2151TimerWrite(opmTimer, opmAddr, val);
            if (opmAddr == 0x01 || opmAddr >= 0x08) x1OpmWrite(opmAddr, val);
        }
            else if (port <= 0x0707) x1CtcWrite(port, val);
            return;
#endif
        case 0x0B00:
            if ((bank_reg ^ val) & 0x1F) { bank_reg = val; updateMap(); }
            else bank_reg = val;
            return;
        case 0x0E00:
            kanjiWrite(port, val); return;
        case 0x0F00:
            if (port >= 0x0FF8 && port <= 0x0FFF) x1FdcWriteIO(port, val);
            return;
        case 0x1000:
            if (pal_b != val) { pal_b = val; x1VideoPaletteChanged(); } return;
        case 0x1100:
            if (pal_r != val) { pal_r = val; x1VideoPaletteChanged(); } return;
        case 0x1200:
            if (pal_g != val) { pal_g = val; x1VideoPaletteChanged(); } return;
        case 0x1300:
            if (priority_reg != val) { priority_reg = val; x1VideoUpdatePriority(); }
            return;
        case 0x1400: case 0x1500: case 0x1600: case 0x1700:
            pcgWrite(port & 0x3FF, val); return;
        case 0x1800:
            if ((port & 0x000F) == 0x00) {
                crtc_index = val & 0x1F;
            } else if ((port & 0x000F) == 0x01) {
                if (crtc_index < 18) crtc_regs[crtc_index] = val;
            }
            return;
        case 0x1900:
            x1SubIoWrite(val); return;
        case 0x1B00:
            x1PsgWrite(val); return;
        case 0x1C00:
            x1PsgSelectAddr(val); return;
        case 0x1A00:
            x1PpiWrite(port, val); return;
        case 0x1D00:
            setRomSelected(true); return;
        case 0x1E00:
            setRomSelected(false); return;
        case 0x1F00:
            if (port >= 0x1F80 && port <= 0x1F8F) { x1DmaWrite(val); return; }
            if ((port >= 0x1F90 && port <= 0x1F93) || (port >= 0x1F98 && port <= 0x1F9B)) { x1SioWrite(port, val); return; }
            if (port >= 0x1FA0 && port <= 0x1FAB) { x1CtcWrite(port, val); return; }
            if ((port & 0xFFF0) == 0x1FD0) {
                if (x1_mode1 != val) {
                    const bool redraw = ((x1_mode1 ^ val) & 0x0F) != 0;
                    x1_mode1 = val;
                    setGvAccess();
                    if (redraw) x1VideoInvalidate();
                }
                return;
            }
            if ((port & 0xFFF0) == 0x1FE0) {
                if (x1_mode2 != val) {
                    bool key = ((x1_mode2 ^ val) & 0x0F) != 0;
                    x1_mode2 = val;
                    x1VideoPaletteChanged();
                    if (key) x1VideoUpdatePriority();
                }
                return;
            }
            return;
        case 0x2000: case 0x2100: case 0x2200: case 0x2300:
        case 0x2400: case 0x2500: case 0x2600: case 0x2700:
        case 0x2800: case 0x2900: case 0x2A00: case 0x2B00:
        case 0x2C00: case 0x2D00: case 0x2E00: case 0x2F00:
            M.avram[port & 0x7FF] = val; x1MarkTx(port); return;
        case 0x3000: case 0x3100: case 0x3200: case 0x3300:
        case 0x3400: case 0x3500: case 0x3600: case 0x3700:
            M.tvram[port & 0x7FF] = val; x1MarkTx(port); return;
        case 0x3800: case 0x3900: case 0x3A00: case 0x3B00:
        case 0x3C00: case 0x3D00: case 0x3E00: case 0x3F00:
            M.kvram[port & 0x7FF] = val; x1MarkTx(port); return;
        default:
            return;
    }
}

volatile int x1_current_scanline = 0;

void x1ScanlineTick(int line) {
    x1_current_scanline = line;
#if X1_OPM_BOARD
    ym2151TimerAdvance(opmTimer, x1_cyc_line, x1OpmCsm);
#endif
    x1SubScanlineTick();
    x1SioScanlineTick();
    x1FdcScanlineTick();
    x1DmaRdy(true);

    static int key_poll_cyc = 0;
    key_poll_cyc += x1_cyc_line;
    if (key_poll_cyc >= 16000) {
        key_poll_cyc -= 16000;
        x1KeyPollTick();
    }

    if (line < x1_disp_lines && !x1_render_skip) {
        x1RenderLine(line);
    }
}

bool x1_render_skip = false;

static uint8_t irq_vector = 0xFF;
static uint16_t irq_ius;
static uint16_t irqSources() {
    uint16_t m = x1SubIrqSources();
    if (x1DmaIrqPending()) m |= 0x0002;
    const uint16_t top = (uint16_t)(irq_ius & (uint16_t)-irq_ius);
    return top ? (uint16_t)(m & (top - 1)) : m;
}
bool x1IrqPending() { return irqSources() != 0; }
void x1IrqAck() {
    const uint16_t m = irqSources();
    if (!m) { irq_vector = 0xFF; return; }
    int src = 0;
    while (!((m >> src) & 1)) src++;
    irq_vector = (src == 1) ? x1DmaIrqAck() : x1SubIrqAckSource(src);
    if (src != 2) irq_ius |= (uint16_t)(1 << src);
}
extern "C" void x1Reti(void) { irq_ius &= (uint16_t)(irq_ius - 1); }
void x1IrqReset() { irq_ius = 0; }
uint8_t x1IrqVector() { return irq_vector; }

uint32_t x1_frame_count;

void x1FrameTick() {
    x1_frame_count++;
    static uint32_t sec_cyc = 0;
    sec_cyc += (uint32_t)x1_cyc_line * x1_lines_frame;
    if (sec_cyc >= 4000000u) {
        sec_cyc -= 4000000u;
        x1RtcTick();
    }
}
