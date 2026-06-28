#include "usb_log.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"       // must precede diskio.h (provides BYTE/UINT/LBA_t)
#include "diskio.h"
#include "hardware/timer.h"
#include "orb_log.h"
#include "tusb.h"

//--------------------------------------------------------------------+
// SPSC log ring: core0 producer (usb_log_write), core1 consumer (usb_log_task).
// Same lock-free discipline as dlog -- aligned 32-bit head/tail loads/stores are
// atomic on M0+, producer and consumer touch different words.
//--------------------------------------------------------------------+
#define ULOG_SIZE 32768u  // power of two
#define ULOG_MASK (ULOG_SIZE - 1u)

static volatile uint8_t ulog_buf[ULOG_SIZE];
static volatile uint32_t ulog_head;  // producer (core0)
static volatile uint32_t ulog_tail;  // consumer (core1)

// ANSI-SGR strip state machine. The dlog ring carries inline color codes meant
// for the UART sink; LOG.TXT must stay clean. We skip ESC '[' ... <final byte>.
// usb_log_write is the single producer (core0 drain), so this static state is not
// shared across cores; it persists across calls so a sequence may span chunks.
enum { ANSI_NORMAL = 0, ANSI_ESC, ANSI_CSI };
static uint8_t s_ansi_state;

void usb_log_write(const uint8_t *data, uint32_t len) {
    uint32_t h = ulog_head;
    uint32_t tail = ulog_tail;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        switch (s_ansi_state) {
            case ANSI_ESC:
                if (c == '[') { s_ansi_state = ANSI_CSI; continue; }
                s_ansi_state = ANSI_NORMAL;  // lone ESC: drop it, keep this byte
                break;
            case ANSI_CSI:
                if (c >= 0x40 && c <= 0x7e) s_ansi_state = ANSI_NORMAL;  // final byte
                continue;                                                // skip params + final
            default:  // ANSI_NORMAL
                if (c == 0x1b) { s_ansi_state = ANSI_ESC; continue; }
                break;
        }
        uint32_t nh = (h + 1u) & ULOG_MASK;
        if (nh == tail) break;  // ring full -> drop the rest
        ulog_buf[h] = c;
        h = nh;
    }
    ulog_head = h;
}

//--------------------------------------------------------------------+
// State (core1 only, except the ring above)
//--------------------------------------------------------------------+
static uint8_t s_dev_addr;   // mounted MSC device address, 0 = none
static bool s_fs_ready;      // FatFs mounted + LOG.TXT open
static bool s_enabled = true;
static FATFS s_fatfs;
static FIL s_file;
static uint32_t s_last_sync_us;
static TU_ATTR_ALIGNED(4) uint8_t s_chunk[512];  // drain buffer (USB transfer src)

void usb_log_set_enabled(bool enabled) { s_enabled = enabled; }

static void drive_path(uint8_t dev_addr, char out[4]) {
    out[0] = (char)('0' + (dev_addr - 1));  // FatFs volume = dev_addr-1
    out[1] = ':';
    out[2] = 0;
}

static bool open_log(void) {
    char path[4];
    drive_path(s_dev_addr, path);
    if (f_mount(&s_fatfs, path, 1) != FR_OK) {
        LOG_ERR(CAT_USBLOG, "f_mount failed");
        return false;
    }
    char fpath[16];
    snprintf(fpath, sizeof(fpath), "%sLOG.TXT", path);  // e.g. "1:LOG.TXT"
    FRESULT fr = f_open(&s_file, fpath, FA_WRITE | FA_OPEN_APPEND);
    if (fr != FR_OK) {
        LOG_ERR(CAT_USBLOG, "f_open %s failed (%d)", fpath, fr);
        f_mount(0, path, 0);
        return false;
    }
    LOG_INFO(CAT_USBLOG, "streaming log to %s (size=%lu)", fpath,
             (unsigned long)f_size(&s_file));
    return true;
}

void usb_log_task(void) {
    if (s_dev_addr == 0 || !s_enabled) return;

    if (!s_fs_ready) {
        if (!open_log()) {
            s_dev_addr = 0;  // give up on this device until re-plugged
            return;
        }
        s_fs_ready = true;
        s_last_sync_us = timer_hw->timerawl;
    }

    // Drain whatever is buffered (one chunk per call; the loop comes back next tick).
    uint32_t tail = ulog_tail;
    uint32_t head = ulog_head;
    if (tail != head) {
        uint32_t n = 0;
        while (tail != head && n < sizeof(s_chunk)) {
            s_chunk[n++] = ulog_buf[tail];
            tail = (tail + 1u) & ULOG_MASK;
        }
        UINT wr = 0;
        FRESULT fr = f_write(&s_file, s_chunk, n, &wr);
        if (fr == FR_OK && wr == n) {
            ulog_tail = tail;  // commit consumption only on full success
        } else {
            LOG_ERR(CAT_USBLOG, "f_write err fr=%d wr=%lu/%lu", fr, (unsigned long)wr,
                    (unsigned long)n);
        }
    }

    // Periodic flush so the on-disk file size/FAT are updated and a pulled stick is
    // readable up to ~1 s ago.
    uint32_t now = timer_hw->timerawl;
    if ((uint32_t)(now - s_last_sync_us) > 1000000u) {
        f_sync(&s_file);
        s_last_sync_us = now;
    }
}

//--------------------------------------------------------------------+
// TinyUSB MSC host mount/unmount (called from tuh_task on core1). Only set flags
// here -- the FatFs work happens in usb_log_task to keep it out of callback context.
//--------------------------------------------------------------------+
void tuh_msc_mount_cb(uint8_t dev_addr) {
    uint32_t bc = tuh_msc_get_block_count(dev_addr, 0);
    uint32_t bs = tuh_msc_get_block_size(dev_addr, 0);
    LOG_INFO(CAT_USBLOG, "stick mounted addr=%d (%lu MB)", dev_addr,
             (unsigned long)((uint64_t)bc * bs / (1024u * 1024u)));
    if (bs != 512) {
        LOG_WARN(CAT_USBLOG, "block size %lu unsupported, ignoring", (unsigned long)bs);
        return;
    }
    s_dev_addr = dev_addr;
    s_fs_ready = false;  // usb_log_task will mount + open
}

void tuh_msc_umount_cb(uint8_t dev_addr) {
    LOG_INFO(CAT_USBLOG, "stick unmounted addr=%d", dev_addr);
    if (dev_addr == s_dev_addr) {
        if (s_fs_ready) {
            char path[4];
            drive_path(s_dev_addr, path);
            f_close(&s_file);
            f_mount(0, path, 0);
        }
        s_dev_addr = 0;
        s_fs_ready = false;
    }
}

//--------------------------------------------------------------------+
// FatFs diskio glue -> TinyUSB MSC host. Blocking: issue the SCSI command then
// pump tuh_task() until the completion callback fires. Safe because usb_log_task
// (and thus f_*) runs from the core1 main loop, not inside an active tuh_task.
//--------------------------------------------------------------------+
static volatile bool s_disk_busy[CFG_TUH_DEVICE_MAX];

static bool disk_io_complete(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data) {
    (void)cb_data;
    s_disk_busy[dev_addr - 1] = false;
    return true;
}

static void wait_for_disk_io(BYTE pdrv) {
    // tuh_task_ext(0, false): non-blocking pump. Under OPT_OS_FREERTOS a plain
    // tuh_task() blocks on the host event queue forever -- here we must keep spinning
    // the host stack until the disk completion callback fires, so force the
    // non-blocking variant. (No-op difference under OPT_OS_PICO.)
    while (s_disk_busy[pdrv]) {
        tuh_task_ext(0, false);
    }
}

DSTATUS disk_status(BYTE pdrv) {
    return tuh_msc_mounted((uint8_t)(pdrv + 1)) ? 0 : STA_NODISK;
}

DSTATUS disk_initialize(BYTE pdrv) {
    (void)pdrv;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    uint8_t const dev_addr = (uint8_t)(pdrv + 1);
    s_disk_busy[pdrv] = true;
    tuh_msc_read10(dev_addr, 0, buff, sector, (uint16_t)count, disk_io_complete, 0);
    wait_for_disk_io(pdrv);
    return RES_OK;
}

#if FF_FS_READONLY == 0
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    uint8_t const dev_addr = (uint8_t)(pdrv + 1);
    s_disk_busy[pdrv] = true;
    tuh_msc_write10(dev_addr, 0, buff, sector, (uint16_t)count, disk_io_complete, 0);
    wait_for_disk_io(pdrv);
    return RES_OK;
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    uint8_t const dev_addr = (uint8_t)(pdrv + 1);
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;  // writes are blocking
        case GET_SECTOR_COUNT:
            *((DWORD *)buff) = (DWORD)tuh_msc_get_block_count(dev_addr, 0);
            return RES_OK;
        case GET_SECTOR_SIZE:
            *((WORD *)buff) = (WORD)tuh_msc_get_block_size(dev_addr, 0);
            return RES_OK;
        case GET_BLOCK_SIZE:
            *((DWORD *)buff) = 1;  // erase block size in sectors
            return RES_OK;
        default:
            return RES_PARERR;
    }
}
