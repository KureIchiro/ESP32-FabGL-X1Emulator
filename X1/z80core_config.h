/* z80core_config.h - X1 / X1turbo: the memory map, I/O and hooks of the Z80 for z80core
   Memory: 4 KB page tables x1_rd_map / x1_wr_map (x1_core.cpp; the X1 has no I/O in the memory space).
   I/O: InZ80 / OutZ80 (X1.ino) see the cycles left in the line (z->icount); OutZ80 takes the cycles the DMA used.
   RETI frees the in-service device of the daisy chain (x1Reti, x1_core.cpp).
   X1_IDLE_SKIP 1: a taken JP / JR / DJNZ back by at most X1_SP_MAXLEN bytes calls the idle-loop skipper (x1_spin.c);
   0 = run every loop instruction by instruction. The results are the same either way. */
#ifndef Z80CORE_CONFIG_H
#define Z80CORE_CONFIG_H
#include <stdint.h>

#ifndef X1_IDLE_SKIP
#define X1_IDLE_SKIP 1
#endif
#define X1_SP_MAXLEN 8

extern uint8_t *x1_rd_map[], *x1_wr_map[];
uint8_t InZ80(uint16_t port);
void OutZ80(uint16_t port, uint8_t value);
void x1Reti(void);
int x1IoPeek(uint16_t port, uint8_t *v);
extern int x1_sp_have;
int x1SpinHook(struct z80 *z, uint16_t b, uint16_t t, int cyc);

#define Z80CORE_RD(a) (x1_rd_map[(a) >> 12][(a) & 0x0FFF])
#define Z80CORE_WR(a, v) (x1_wr_map[(a) >> 12][(a) & 0x0FFF] = (v))
#define Z80CORE_IN(p) ({ Z80CORE_SYNC_OUT(); const uint8_t vi_ = InZ80(p); Z80CORE_SYNC_IN(); vi_; })
#define Z80CORE_OUT(p, v) do { Z80CORE_SYNC_OUT(); OutZ80((p), (v)); Z80CORE_SYNC_IN(); } while (0)
#define Z80CORE_RETI() x1Reti()
#if X1_IDLE_SKIP
#define Z80CORE_SPIN(b, t) do { if ((uint16_t)((b) - (t)) <= X1_SP_MAXLEN) { Z80CORE_R_OUT(); cyc = x1SpinHook(z, (b), (t), cyc); Z80CORE_R_IN(); } } while (0)
#define Z80CORE_ON_INT() (x1_sp_have = 0)
#endif

#endif
