// ============================================================================
//  X1 FDC: MB8877 (WD1793/WD2793系)
//  自作MSXエミュ(ESP32-FabGL-MSX1Emulator)のWD2793実装を元に、ディスクを D88 のまま
//  扱うように作り直したもの。
//
//  ・D88 イメージをそのまま PSRAM に置き、トラックごとのセクタの並び(ID の C/H/R/N、
//    サイズ、削除マーク、CRC エラーなどの状態)を D88 のヘッダから引く。
//    以前は 16セクタ×256バイトの決まった形に並べ直していたため、セクタ数・番号・
//    サイズが普通でないディスク(コピープロテクト)の情報が失われていた。
//  ・セクタは実機と同じく、ヘッドの位置のトラックで、ID の C がトラックレジスタ、
//    R がセクタレジスタと一致するものを回転の順に探す(同じ R が2つあるときは
//    回転位置で先に来る方)。見つからなければ Record Not Found。
//  ・ヘッドの位置はドライブごとに持ち、トラックレジスタとは別(Step で U を付けない
//    ときはヘッドだけ動く。Verify はヘッドのトラックの ID の C とトラックレジスタを比べる)。
//  ・Read Address・Read Track・Write Track(フォーマット)にも対応する(Write Track は
//    セクタの並びは変えず、データ部を書かれた値で埋める簡易版)。
//  ・時間はモーターの起動/停止遅延(eX1由来)、最初の DRQ までの遅れ、インデックス
//    パルス(300rpm)だけ再現する。
// ============================================================================
// 【最適化】Arduinoのesp32コアは既定で -Os。FDC はフレームごとに
// 数万回まわるため、この翻訳単位だけ O3 に引き上げる。
// (X1.ino:20 の #pragma は .ino の翻訳単位にしか効かない)
#pragma GCC optimize("O3")

#include "x1.h"
#include <string.h>
#include <stdio.h>

struct WD2793 {
    uint8_t Command;
    uint8_t Status;
    uint8_t Track;
    uint8_t Sector;
    uint8_t Data;
    uint8_t Side;
};
static WD2793 fdc = { 0x00, 0x00, 0x00, 0x01, 0x00, 0x00 };

static X1Drive *drives;
static int      numDrives;
static uint8_t  current_drive;
static uint8_t  head_pos[4];
static uint8_t  rot_pos[4];

enum Mode { M_IDLE, M_READ, M_WRITE, M_READADDR, M_READTRACK, M_WRITETRACK, M_SEARCH };
static Mode     mode;
static bool     typeI = true;
static bool     head_loaded;
static uint8_t *data_ptr;
static int      data_len, data_index;
static uint32_t cur_hdr;
static uint8_t  end_status;
static bool     fdc_intrq, intrq_hold;
static int      fdc_step_direction = -1;
static bool     fdc_multi_sector;
static uint8_t  fdc_ctrl_reg;
static uint8_t  id_buf[6];
static uint8_t  track_buf[6400];
#define FDC_DRQ_DELAY_CYC 750
static int      fdc_drq_delay;
static const int FDC_STEP_CYC[4] = { 24000, 48000, 80000, 120000 };
#define FDC_SETTLE_CYC 60000
static int      fdc_seek_busy;
#define FDC_WRITE_LOST_CYC 2000
static int      fdc_write_idle;
#define FDC_READ_LOST_CYC  2000
#define FDC_BYTE_CYC       128
static int      fdc_read_idle;
static int      fdc_read_drain;
static int      fdc_dma_cyc;
#define FDC_RNF_CYC 4000000
static int      fdc_search_cyc;
static uint8_t  search_status;
#define TRACK_BYTES 6250
#define MAX_HEAD    83

static bool     motor_on;
static bool     motor_spinning_up;
static bool     motor_stopping;
static int      motor_timer_ms;
static uint32_t fdc_ms;

void x1FdcInit(X1Drive *d, int n) {
    drives = d;
    numDrives = n;
    memset(head_pos, 0, sizeof(head_pos));
    memset(rot_pos, 0, sizeof(rot_pos));
    x1FdcReset();
}

void x1FdcReset() {
    memset(&fdc, 0, sizeof(fdc));
    fdc.Sector = 1;
    current_drive = 0;
    mode = M_IDLE;
    typeI = true;
    head_loaded = false;
    data_ptr = nullptr;
    data_len = data_index = 0;
    end_status = 0;
    fdc_intrq = intrq_hold = false;
    fdc_seek_busy = 0;
    fdc_step_direction = -1;
    fdc_multi_sector = false;
    fdc_ctrl_reg = 0;
    fdc_drq_delay = 0;
    fdc_read_idle = fdc_read_drain = 0;
    fdc_dma_cyc = 0;
    motor_on =motor_spinning_up = motor_stopping = false;
    motor_timer_ms = 0;
}

static inline uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t rd16le(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

enum { SH_C = 0, SH_H = 1, SH_R = 2, SH_N = 3, SH_NSEC = 4, SH_DEL = 7, SH_STAT = 8, SH_SIZE = 14 };
#define ST_ID_CRC   0xA0
#define ST_DATA_CRC 0xB0
#define ST_NO_AM    0xE0
#define ST_NO_DAM   0xF0

static inline X1Drive *curDrive() {
    return (current_drive < numDrives && drives[current_drive].inserted) ? &drives[current_drive] : nullptr;
}
static inline int trackSlot(int head, int side) { return head * 2 + side; }

static uint32_t sectorHeader(const X1Drive &d, int slot, int n) {
    if (slot < 0 || slot >= X1_D88_TRACKS || n >= d.trk_nsec[slot]) return 0;
    uint32_t pos = d.trk_ofs[slot];
    for (int i = 0; i < n; i++) pos += 16 + rd16le(&d.buffer[pos + SH_SIZE]);
    return pos;
}

bool x1LoadD88(X1Drive &drv, const uint8_t *fileBuf, uint32_t fileSize) {
    if (fileSize < 0x2B0 || fileSize > X1_D88_MAX || !drv.buffer) return false;
    if (fileBuf != drv.buffer) memcpy(drv.buffer, fileBuf, fileSize);
    const uint8_t *b = drv.buffer;
    drv.size = fileSize;
    drv.protect = (b[0x1A] & 0x10) != 0;
    uint32_t end = rd32le(&b[0x1C]);
    if (end == 0 || end > fileSize) end = fileSize;
    for (int t = 0; t < X1_D88_TRACKS; t++) {
        uint32_t ofs = rd32le(&b[0x20 + t * 4]);
        drv.trk_ofs[t] = 0;
        drv.trk_nsec[t] = 0;
        if (ofs < 0x20 + 4 || ofs + 16 > end) continue;
        uint32_t next = end;
        for (int u = 0; u < X1_D88_TRACKS; u++) {
            uint32_t o = rd32le(&b[0x20 + u * 4]);
            if (o > ofs && o < next) next = o;
        }
        int want = rd16le(&b[ofs + SH_NSEC]);
        if (want <= 0 || want > 255) want = 255;
        uint32_t pos = ofs;
        int n = 0;
        while (n < want && pos + 16 <= next) {
            uint32_t sz = rd16le(&b[pos + SH_SIZE]);
            if (pos + 16 + sz > next) break;
            pos += 16 + sz;
            n++;
        }
        if (n > 0) { drv.trk_ofs[t] = ofs; drv.trk_nsec[t] = (uint8_t)n; }
    }
    drv.has_d88_header = true;
    drv.inserted = true;
    return true;
}

uint32_t x1RawToD88(uint8_t *dst, uint32_t dstMax, const uint8_t *raw, uint32_t rawSize) {
    const uint32_t perCyl = 2u * 16u * 256u;
    if (rawSize == 0 || rawSize % perCyl) return 0;
    const int cyl = (int)(rawSize / perCyl);
    if (cyl > X1_D88_TRACKS / 2) return 0;
    const int tracks = cyl * 2;
    const uint32_t size = 0x2B0 + (uint32_t)tracks * 16u * (16u + 256u);
    if (size > dstMax) return 0;
    memset(dst, 0, 0x2B0);
    dst[0x1B] = cyl > 42 ? 0x10 : 0x00;
    dst[0x1C] = (uint8_t)size; dst[0x1D] = (uint8_t)(size >> 8); dst[0x1E] = (uint8_t)(size >> 16); dst[0x1F] = (uint8_t)(size >> 24);
    uint32_t pos = 0x2B0;
    for (int t = 0; t < tracks; t++) {
        dst[0x20 + t * 4] = (uint8_t)pos; dst[0x21 + t * 4] = (uint8_t)(pos >> 8);
        dst[0x22 + t * 4] = (uint8_t)(pos >> 16); dst[0x23 + t * 4] = (uint8_t)(pos >> 24);
        for (int s = 0; s < 16; s++) {
            uint8_t *h = dst + pos;
            memset(h, 0, 16);
            h[SH_C] = (uint8_t)(t >> 1); h[SH_H] = (uint8_t)(t & 1); h[SH_R] = (uint8_t)(s + 1); h[SH_N] = 1;
            h[SH_NSEC] = 16;
            h[SH_SIZE] = 0x00; h[SH_SIZE + 1] = 0x01;
            memmove(h + 16, raw + ((uint32_t)t * 16 + s) * 256, 256);
            pos += 16 + 256;
        }
    }
    return size;
}

uint32_t x1WriteBackD88(const X1Drive &drv, uint8_t *outBuf, uint32_t outBufSize) {
    if (!drv.has_d88_header || drv.size == 0 || drv.size > outBufSize) return 0;
    memcpy(outBuf, drv.buffer, drv.size);
    return drv.size;
}

static uint16_t crc16(uint16_t crc, const uint8_t *p, int n) {
    while (n-- > 0) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}
static uint16_t markCrc(uint8_t mark, const uint8_t *p, int n) {
    uint8_t pre[4] = { 0xA1, 0xA1, 0xA1, mark };
    return crc16(crc16(0xFFFF, pre, 4), p, n);
}

static uint32_t findSector(uint8_t sector, bool cmpSide, uint8_t side, bool *crcErr) {
    X1Drive *d = curDrive();
    *crcErr = false;
    if (!d) return 0;
    int slot = trackSlot(head_pos[current_drive], fdc.Side);
    int n = (slot < X1_D88_TRACKS) ? d->trk_nsec[slot] : 0;
    for (int i = 0; i < n; i++) {
        int k = (rot_pos[current_drive] + i) % n;
        uint32_t h = sectorHeader(*d, slot, k);
        const uint8_t *sh = &d->buffer[h];
        if (sh[SH_STAT] == ST_NO_AM) continue;
        if (sh[SH_C] != fdc.Track || sh[SH_R] != sector) continue;
        if (cmpSide && sh[SH_H] != side) continue;
        if (sh[SH_STAT] == ST_ID_CRC) { *crcErr = true; continue; }
        rot_pos[current_drive] = (uint8_t)((k + 1) % n);
        return h;
    }
    return 0;
}

static void finish(uint8_t status) {
    mode = M_IDLE;
    fdc.Status = status;
    fdc_intrq = true;
}

static void startSector(bool write) {
    bool cmpSide = (fdc.Command & 0x02) != 0;
    uint8_t side = (fdc.Command >> 3) & 1;
    bool crcErr;
    uint32_t h = findSector(fdc.Sector, cmpSide, side, &crcErr);
    X1Drive *d = curDrive();
    if (!h || (!write && d->buffer[h + SH_STAT] == ST_NO_DAM)) {
        mode = M_SEARCH;
        fdc.Status = 0x01;
        data_len = data_index = 0;
        search_status = 0x10 | (crcErr ? 0x08 : 0x00);
        fdc_search_cyc = FDC_RNF_CYC;
        return;
    }
    uint8_t *sh = &d->buffer[h];
    cur_hdr = h;
    data_ptr = sh + 16;
    data_len = rd16le(&sh[SH_SIZE]);
    data_index = 0;
    end_status = 0;
    if (!write) {
        if (sh[SH_DEL] & 0x10) end_status |= 0x20;
        if (sh[SH_STAT] == ST_DATA_CRC) end_status |= 0x08;
    }
    mode = write ? M_WRITE : M_READ;
    fdc.Status = 0x03;
    fdc_drq_delay = FDC_DRQ_DELAY_CYC;
    fdc_read_idle = fdc_read_drain = 0;
    fdc_dma_cyc = 0;
}

static void sectorDone(bool write) {
    if (write) {
        uint8_t *sh = &curDrive()->buffer[cur_hdr];
        sh[SH_STAT] = 0x00;
        sh[SH_DEL] = (fdc.Command & 0x01) ? 0x10 : 0x00;
    }
    if (end_status & 0x08) { finish(end_status); return; }
    if (fdc_multi_sector) {
        fdc.Sector++;
        uint8_t keep = end_status;
        startSector(write);
        if (mode == M_SEARCH) search_status = (search_status & ~0x20) | (keep & 0x20);
        return;
    }
    finish(end_status);
}

static int buildTrack() {
    X1Drive *d = curDrive();
    int len = 0;
    auto put = [&](uint8_t v, int n) { while (n-- > 0 && len < (int)sizeof(track_buf)) track_buf[len++] = v; };
    put(0x4E, 80); put(0x00, 12); put(0xC2, 3); put(0xFC, 1); put(0x4E, 50);
    int slot = trackSlot(head_pos[current_drive], fdc.Side);
    int n = (d && slot < X1_D88_TRACKS) ? d->trk_nsec[slot] : 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *sh = &d->buffer[sectorHeader(*d, slot, i)];
        if (sh[SH_STAT] == ST_NO_AM) continue;
        put(0x00, 12); put(0xA1, 3); put(0xFE, 1);
        uint16_t c = markCrc(0xFE, sh, 4);
        for (int k = 0; k < 4; k++) put(sh[k], 1);
        put(c >> 8, 1); put(c & 0xFF, 1);
        put(0x4E, 22);
        if (sh[SH_STAT] == ST_NO_DAM) continue;
        uint8_t dam = (sh[SH_DEL] & 0x10) ? 0xF8 : 0xFB;
        int sz = rd16le(&sh[SH_SIZE]);
        put(0x00, 12); put(0xA1, 3); put(dam, 1);
        for (int k = 0; k < sz && len < (int)sizeof(track_buf); k++) track_buf[len++] = sh[16 + k];
        uint16_t dc = markCrc(dam, &sh[16], sz);
        put(dc >> 8, 1); put(dc & 0xFF, 1);
        put(0x4E, 54);
    }
    put(0x4E, TRACK_BYTES - len);
    return len > TRACK_BYTES ? len : TRACK_BYTES;
}

static void applyWriteTrack() {
    X1Drive *d = curDrive();
    int slot = trackSlot(head_pos[current_drive], fdc.Side);
    if (!d || slot >= X1_D88_TRACKS) return;
    uint8_t fill = 0xE5;
    for (int i = 0; i + 1 < data_len; i++)
        if (track_buf[i] == 0xFB || track_buf[i] == 0xF8) { fill = track_buf[i + 1]; break; }
    for (int i = 0; i < d->trk_nsec[slot]; i++) {
        uint8_t *sh = &d->buffer[sectorHeader(*d, slot, i)];
        memset(sh + 16, fill, rd16le(&sh[SH_SIZE]));
        sh[SH_STAT] = 0x00;
        sh[SH_DEL] = 0x00;
    }
}

static bool driveReady() {
    return motor_on && curDrive() != nullptr;
}

static void typeICommand(uint8_t cmd) {
    bool update = (cmd & 0x10) != 0;
    uint8_t &hp = head_pos[current_drive & 3];
    auto move = [&](int dir) {
        int h = hp + dir;
        hp = (uint8_t)(h < 0 ? 0 : h > MAX_HEAD ? MAX_HEAD : h);
    };
    fdc.Status = 0;
    if (cmd & 0x08) head_loaded = true;
    int steps = 1;
    switch (cmd & 0xE0) {
        case 0x00:
            if (!(cmd & 0x10)) {
                steps = hp;
                hp = 0;
                fdc.Track = 0;
                fdc_step_direction = -1;
            } else {
                int diff = (int)fdc.Data - (int)fdc.Track;
                if (diff) fdc_step_direction = diff > 0 ? 1 : -1;
                steps = diff < 0 ? -diff : diff;
                move(diff);
                fdc.Track = fdc.Data;
            }
            break;
        case 0x20: move(fdc_step_direction); if (update) fdc.Track += fdc_step_direction; break;
        case 0x40: fdc_step_direction = 1;  move(1);  if (update) fdc.Track++; break;
        case 0x60: fdc_step_direction = -1; move(-1); if (update && fdc.Track > 0) fdc.Track--; break;
    }
    if (cmd & 0x04) {
        head_loaded = true;
        bool ok = false, crc = false;
        X1Drive *d = curDrive();
        int slot = trackSlot(hp, fdc.Side);
        if (driveReady() && d && slot < X1_D88_TRACKS) {
            for (int i = 0; i < d->trk_nsec[slot] && !ok; i++) {
                const uint8_t *sh = &d->buffer[sectorHeader(*d, slot, i)];
                if (sh[SH_STAT] == ST_NO_AM || sh[SH_C] != fdc.Track) continue;
                if (sh[SH_STAT] == ST_ID_CRC) crc = true; else ok = true;
            }
        }
        if (!ok) fdc.Status |= 0x10 | (crc ? 0x08 : 0x00);
    }
    mode = M_IDLE;
    typeI = true;
    int busy = steps * FDC_STEP_CYC[cmd & 3] + ((cmd & 0x04) ? FDC_SETTLE_CYC : 0);
    fdc_seek_busy = busy < 500 ? 500 : busy;
}

static void executeCommand(uint8_t cmd) {
    fdc.Command = cmd;
    fdc_intrq = intrq_hold = false;
    data_index = 0;
    fdc_dma_cyc = 0;
    fdc_seek_busy = 0;
    fdc_write_idle = 0;

    if ((cmd & 0xF0) == 0xD0) {
        if (mode == M_IDLE) { fdc.Status = 0; typeI = true; }
        mode = M_IDLE;
        if (cmd & 0x08) fdc_intrq = intrq_hold = true;
        else if (cmd & 0x04) fdc_intrq = true;
        return;
    }
    if ((cmd & 0x80) == 0) { typeICommand(cmd); return; }

    typeI = false;
    head_loaded = true;
    mode = M_IDLE;
    if (!driveReady()) { finish(0x80); return; }

    if ((cmd & 0xC0) == 0x80) {
        fdc_multi_sector = (cmd & 0x10) != 0;
        bool write = (cmd & 0x20) != 0;
        if (write && curDrive()->protect) { finish(0x40); return; }
        startSector(write);
        return;
    }
    X1Drive *d = curDrive();
    int slot = trackSlot(head_pos[current_drive], fdc.Side);
    switch (cmd & 0xF0) {
        case 0xC0: {
            int n = (slot < X1_D88_TRACKS) ? d->trk_nsec[slot] : 0;
            for (int i = 0; i < n; i++) {
                int k = (rot_pos[current_drive] + i) % n;
                const uint8_t *sh = &d->buffer[sectorHeader(*d, slot, k)];
                if (sh[SH_STAT] == ST_NO_AM) continue;
                rot_pos[current_drive] = (uint8_t)((k + 1) % n);
                memcpy(id_buf, sh, 4);
                uint16_t c = markCrc(0xFE, sh, 4);
                id_buf[4] = c >> 8; id_buf[5] = c & 0xFF;
                data_ptr = id_buf;
                data_len = 6;
                end_status = (sh[SH_STAT] == ST_ID_CRC) ? 0x08 : 0x00;
                mode = M_READADDR;
                fdc.Status = 0x03;
                fdc_drq_delay = FDC_DRQ_DELAY_CYC;
                return;
            }
            finish(0x10);
            return;
        }
        case 0xE0:
            data_ptr = track_buf;
            data_len = buildTrack();
            mode = M_READTRACK;
            fdc.Status = 0x03;
            fdc_drq_delay = FDC_DRQ_DELAY_CYC;
            return;
        default:
            if (d->protect) { finish(0x40); return; }
            data_ptr = track_buf;
            data_len = TRACK_BYTES;
            mode = M_WRITETRACK;
            fdc.Status = 0x03;
            fdc_drq_delay = FDC_DRQ_DELAY_CYC;
            return;
    }
}

static uint8_t readStatus() {
    if (!intrq_hold) fdc_intrq = false;
    bool ready = driveReady();
    if (typeI) {
        uint8_t s = fdc.Status & 0x18;
        if (!ready) s |= 0x80;
        X1Drive *d = curDrive();
        if (d && d->protect) s |= 0x40;
        if (head_loaded && ready) s |= 0x20;
        if (head_pos[current_drive & 3] == 0) s |= 0x04;
        if (ready && (fdc_ms % 200) < 4) s |= 0x02;
        if (fdc_seek_busy) s |= 0x01;
        return s;
    }
    uint8_t s = fdc.Status;
    if (mode != M_IDLE) {
        s |= 0x01;
        if (data_index < data_len && fdc_drq_delay == 0) s |= 0x02;
        else s &= ~0x02;
    } else {
        s &= ~0x03;
    }
    return (uint8_t)((s & 0x7F) | (ready ? 0x00 : 0x80));
}

bool x1FdcDrq() {
    if (mode == M_IDLE || fdc_drq_delay > 0 || data_index >= data_len) return false;
    if (fdc_dma_cyc < FDC_BYTE_CYC) return false;
    return mode == M_READ || mode == M_READADDR || mode == M_READTRACK || mode == M_WRITE || mode == M_WRITETRACK;
}

void x1FdcScanlineTick() {
    if (mode != M_IDLE && fdc_drq_delay <= 0 && data_index < data_len) {
        fdc_dma_cyc += x1_cyc_line;
        if (fdc_dma_cyc > 3 * FDC_BYTE_CYC) fdc_dma_cyc = 3 * FDC_BYTE_CYC;
    }
    if (fdc_drq_delay > 0) { fdc_drq_delay -= x1_cyc_line; if (fdc_drq_delay < 0) fdc_drq_delay = 0; }
    else if ((mode == M_WRITE || mode == M_WRITETRACK) && data_index == 0) {
        fdc_write_idle += x1_cyc_line;
        if (fdc_write_idle >= FDC_WRITE_LOST_CYC) { fdc_write_idle = 0; finish(0x04); }
    } else if (mode == M_READ && fdc_read_drain > 0) {
        fdc_read_drain -= x1_cyc_line;
        if (fdc_read_drain <= 0) { fdc_read_drain = 0; finish((uint8_t)(end_status | 0x04)); }
    } else if (mode == M_READ && data_index < data_len) {
        fdc_read_idle += x1_cyc_line;
        if (fdc_read_idle >= FDC_READ_LOST_CYC) {
            const int rest = (data_len - data_index) * FDC_BYTE_CYC - FDC_READ_LOST_CYC;
            data_index = data_len;
            fdc.Status |= 0x04;
            fdc_read_drain = rest > 1 ? rest : 1;
        }
    }
    if (fdc_seek_busy > 0) { fdc_seek_busy -= x1_cyc_line; if (fdc_seek_busy <= 0) { fdc_seek_busy = 0; fdc_intrq = true; } }
    if (mode == M_SEARCH) { fdc_search_cyc -= x1_cyc_line; if (fdc_search_cyc <= 0) finish(search_status); }
}

static uint8_t readData() {
    if ((mode == M_READ || mode == M_READADDR || mode == M_READTRACK) && fdc_drq_delay == 0 && data_index < data_len) {
        uint8_t val = data_ptr[data_index++];
        if (fdc_dma_cyc >= FDC_BYTE_CYC) fdc_dma_cyc -= FDC_BYTE_CYC;
        fdc.Data = val;
        fdc_read_idle = 0;
        if (data_index >= data_len) {
            if (mode == M_READ) sectorDone(false);
            else if (mode == M_READADDR) { fdc.Sector = id_buf[0]; finish(end_status); }
            else finish(0);
        }
        return val;
    }
    return fdc.Data;
}

static void writeData(uint8_t val) {
    fdc.Data = val;
    if ((mode == M_WRITE || mode == M_WRITETRACK) && fdc_drq_delay == 0 && data_index < data_len) {
        data_ptr[data_index++] = val;
        if (fdc_dma_cyc >= FDC_BYTE_CYC) fdc_dma_cyc -= FDC_BYTE_CYC;
        if (data_index >= data_len) {
            if (mode == M_WRITE) sectorDone(true);
            else { applyWriteTrack(); finish(0); }
        }
    }
}

uint8_t x1FdcReadIO(uint16_t port) {
    switch (port & 0xFF) {
        case 0xF8: return readStatus();
        case 0xF9: return fdc.Track;
        case 0xFA: return fdc.Sector;
        case 0xFB: return readData();
        default:   return 0xFF;
    }
}

void x1FdcWriteIO(uint16_t port, uint8_t val) {
    switch (port & 0xFF) {
        case 0xF8: executeCommand(val); break;
        case 0xF9: fdc.Track = val; break;
        case 0xFA: fdc.Sector = val; break;
        case 0xFB: writeData(val); break;
        case 0xFC: {
            uint8_t drv = val & 0x03;
            if (drv >= numDrives) drv = 0;
            if (drv != current_drive) rot_pos[drv] = 0;
            current_drive = drv;
            fdc.Side = (val >> 4) & 0x01;

            bool new_motor_req = (val & 0x80) != 0;
            if (!(fdc_ctrl_reg & 0x80) && new_motor_req) {
                motor_stopping = false;
                motor_spinning_up = true;
                motor_timer_ms = 560;
            } else if ((fdc_ctrl_reg & 0x80) && !new_motor_req) {
                motor_spinning_up = false;
                motor_stopping = true;
                motor_timer_ms = 1500;
            }
            fdc_ctrl_reg = val;
            break;
        }
        default: break;
    }
}

void x1FdcTick1ms() {
    fdc_ms++;
    if (motor_spinning_up) {
        if (--motor_timer_ms <= 0) {
            motor_on = true;
            motor_spinning_up = false;
        }
    } else if (motor_stopping) {
        if (--motor_timer_ms <= 0) {
            motor_on = false;
            motor_stopping = false;
        }
    }
}

void x1FdcDebugDump(char *buf, int bufSize) {
    snprintf(buf, bufSize,
             "cmd=%02X st=%02X trk=%d head=%d sec=%d side=%d drv=%d ins=%d motor=%d spin=%d stop=%d mode=%d idx=%d/%d",
             fdc.Command, fdc.Status, fdc.Track, head_pos[current_drive & 3], fdc.Sector, fdc.Side, current_drive,
             curDrive() != nullptr, motor_on, motor_spinning_up, motor_stopping, (int)mode, data_index, data_len);
}
