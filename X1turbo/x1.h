#pragma once
#include <Arduino.h>

#define X1_SCREEN_W 640
#define X1_SCREEN_H 400
#define X1_ROW_BYTES (X1_SCREEN_W / 2)
#define X1_CHAR_H   8

#define X1_CPU_CLOCK       4000000
#define X1_CYCLES_PER_LINE 250
#define X1_LINES_PER_FRAME 262
#define X1_MAX_LINES       600
extern int  x1_cyc_line;
extern int  x1_lines_frame;
extern int  x1_disp_lines;
extern bool x1_hireso;
void x1FrameStart();

#define X1_KEY_POLL_HZ     250

#define GVRAM_PLANE_SZ 0x4000
#define GVRAM_B_OFS    0x0000
#define GVRAM_R_OFS    (1 * GVRAM_PLANE_SZ + 32)
#define GVRAM_G_OFS    (2 * GVRAM_PLANE_SZ + 64)
#define GVRAM_PAGE_SZ  (3 * GVRAM_PLANE_SZ + 96)
#define GVRAM_ALLOC_SZ (2 * GVRAM_PAGE_SZ)

#define X1_RAM_PAGE_SHIFT 14
#define X1_RAM_PAGE_SZ    (1u << X1_RAM_PAGE_SHIFT)
#define X1_RAM_PAGE_MASK  (X1_RAM_PAGE_SZ - 1)
#define X1_RAM_PAGES      4
#define X1_EXTRAM_BANKS   16
#define X1_IPL_SIZE       0x8000
#define X1_KANJI_SIZE     0x4BC00

#define X1_PROFILE
#ifdef X1_PROFILE
static inline uint32_t x1Ccount() {
#ifdef ARDUINO
    uint32_t c;
    __asm__ __volatile__("rsr %0, ccount" : "=a"(c));
    return c;
#else
    return 0;
#endif
}
extern uint32_t x1_prof_render;
extern uint32_t x1_prof_rd, x1_prof_bmp, x1_prof_txt;
void x1MemBench();
#endif

struct X1Memory {
    uint8_t *ram_page[X1_RAM_PAGES];
    uint8_t *ipl_rom;
    uint8_t *cgrom;
    uint8_t *kanji;
    uint8_t *tvram;
    uint8_t *avram;
    uint8_t *kvram;
    uint8_t *pcg[3];
    uint8_t *gvram;
    uint8_t *extram;
};

extern "C" {
extern uint8_t *x1_rd_map[16];
extern uint8_t *x1_wr_map[16];
}

void x1Init(const X1Memory &mem);
void x1Reset();
void x1KanjiDeinterleave(uint8_t *kanji);
extern X1Memory M;

extern "C" {
    uint8_t x1ReadByte(uint16_t addr);
    void    x1WriteByte(uint16_t addr, uint8_t val);
    uint8_t x1ReadIO(uint16_t port);
    void    x1WriteIO(uint16_t port, uint8_t val);
}

void x1ScanlineTick(int line);
bool x1IrqPending();
void x1IrqAck();
uint8_t x1IrqVector();
void x1FrameTick();

uint8_t x1SubIoRead();
void    x1SubIoWrite(uint8_t data);
uint8_t x1PpiRead(uint16_t port);
void    x1PpiWrite(uint16_t port, uint8_t val);
uint8_t x1CtcRead(uint16_t port);
void    x1CtcWrite(uint16_t port, uint8_t val);
void    x1SubScanlineTick();
bool    x1SubIrqPending();
uint8_t x1SubIrqAck();
void    x1KeyPollTick();
#define X1_TURBO_KEYBOARD
void    x1RtcTick();

#define X1_KEY_ROWS  16
#define X1K_TEN0     0x60
#define X1K_CTRL     0x70
#define X1K_SHIFT    0x71
#define X1K_KANA     0x72
#define X1K_CAPS     0x73
#define X1K_GRAPH    0x74
#define X1K_F1       0x78
#define X1K_TENCOMMA 0x7D
extern volatile uint8_t x1_key_matrix[X1_KEY_ROWS];

void x1PsgWrite(uint8_t val);
void x1PsgSelectAddr(uint8_t addr);
uint8_t x1PsgRead();
#ifndef X1_OPM_BOARD
#define X1_OPM_BOARD 1
#endif
#define X1_OPM_CLOCK 4000000
void x1OpmWrite(uint8_t reg, uint8_t val);
void x1OpmCsm(uint32_t at);
extern volatile int32_t x1_mouse_dx, x1_mouse_dy;
extern volatile uint8_t x1_mouse_buttons;
void    x1SioReset();
void    x1SioScanlineTick();
uint8_t x1SioRead(uint16_t port);
void    x1SioWrite(uint16_t port, uint8_t val);
bool    x1SioIrqPending();
uint8_t x1SioIrqAck();

void    x1DmaReset();
uint8_t x1DmaRead();
void    x1DmaWrite(uint8_t val);
void    x1DmaRdy(bool ready);
bool    x1DmaIrqPending();
uint8_t x1DmaIrqAck();
int     x1DmaTakeCycles();

#define X1_FDC_TRACKS       40
#define X1_FDC_SIDES        2
#define X1_FDC_SECTORS      16
#define X1_FDC_SECTOR_SIZE  256
#define X1_D88_MAX          (1024u * 1024u)
#define X1_D88_TRACKS       164

struct X1Drive {
    uint8_t *buffer;
    uint32_t size;
    bool     inserted;
    bool     has_d88_header;
    bool     protect;
    uint32_t trk_ofs[X1_D88_TRACKS];
    uint8_t  trk_nsec[X1_D88_TRACKS];
};

void x1FdcInit(X1Drive *drives, int numDrives);
void x1FdcReset();
uint8_t x1FdcReadIO(uint16_t port);
void x1FdcWriteIO(uint16_t port, uint8_t val);
void x1FdcTick1ms();
void x1FdcScanlineTick();
bool x1FdcDrq();

bool x1LoadD88(X1Drive &drv, const uint8_t *fileBuf, uint32_t fileSize);
uint32_t x1WriteBackD88(const X1Drive &drv, uint8_t *outBuf, uint32_t outBufSize);
uint32_t x1RawToD88(uint8_t *dst, uint32_t dstMax, const uint8_t *raw, uint32_t rawSize);
void x1FdcDebugDump(char *buf, int bufSize);

extern volatile int x1_current_scanline;

int x1LineCycle();
extern uint32_t x1_frame_count;

extern uint8_t crtc_regs[18];
extern uint8_t pal_b, pal_r, pal_g;
extern uint8_t priority_reg;
extern bool hres_mode;
extern uint8_t x1_mode1;
extern uint8_t x1_mode1_eff;
extern uint8_t x1_mode2;

void x1SetPortCBits(uint8_t bit5, uint8_t bit6);

void x1RenderLine(int y);
void x1VideoBuildTables();
void x1VideoUpdatePriority();
void x1VideoPaletteChanged();
void x1VideoFrameStart();
extern uint32_t x1_vram_gen;
extern uint32_t x1_gv_gen[2][512];
extern uint32_t x1_tx_gen[128];
extern uint32_t x1_pcg_gen;
extern uint32_t x1_lines_drawn;
extern uint32_t x1_lines_txtsame;
extern bool     x1_render_skip;
static inline void x1MarkGv(int page, uint16_t ofs) { x1_gv_gen[page][(ofs >> 5) & 511] = ++x1_vram_gen; }
static inline void x1MarkTx(uint16_t ofs) { x1_tx_gen[(ofs >> 4) & 127] = ++x1_vram_gen; }
void x1VideoInvalidate();

extern uint8_t **x1_vp_rows;
void x1VideoSetOutput(uint8_t **vpRows);
void x1VideoUnmapRows();
void x1VideoResume();
void x1SetPaletteEntry(int index, uint8_t r, uint8_t g, uint8_t b);
