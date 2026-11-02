#pragma once
#include <stdint.h>

#ifndef OPM_FB_CLIP_ENABLE
#define OPM_FB_CLIP_ENABLE  1
#endif
#ifndef OPM_FB_CLIP_MAX
#define OPM_FB_CLIP_MAX     192
#endif
#ifndef OPM_FB_CLIP_MAX_LO
#define OPM_FB_CLIP_MAX_LO  128
#endif
#ifndef OPM_FB_CLIP_NOISE
#define OPM_FB_CLIP_NOISE   352
#endif
#ifndef OPM_FB_CLIP_LO_HZ
#define OPM_FB_CLIP_LO_HZ   300
#endif
#ifndef OPM_FB_CLIP_HI_HZ
#define OPM_FB_CLIP_HI_HZ   1800
#endif
#ifndef OPM_FB_CLIP_RATE
#define OPM_FB_CLIP_RATE    30000
#endif

struct Ym2151;

Ym2151 *ym2151Create(uint32_t clock, uint32_t rate);
void    ym2151Reset(Ym2151 *opm);
void    ym2151WriteReg(Ym2151 *opm, uint8_t reg, uint8_t val);
void    ym2151Generate(Ym2151 *opm, int32_t *out, int n);
void    ym2151CsmKeyOn(Ym2151 *opm);

struct Ym2151Timer {
  uint16_t na; uint8_t nb;
  uint8_t  ctrl, status;
  int32_t  cntA, cntB;
};
void ym2151TimerReset(Ym2151Timer &t);
void ym2151TimerWrite(Ym2151Timer &t, uint8_t reg, uint8_t val);
void ym2151TimerAdvance(Ym2151Timer &t, uint32_t clocks, void (*csm)(uint32_t at) = nullptr);
