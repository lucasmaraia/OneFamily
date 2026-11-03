/*
 * OneFamily DualSense Bluetooth Driver Subsystem
 *
 * Implements native wireless DualSense support via /dev/bt kernel allowlist patching,
 * 78-byte extended HID report 0x31 decoding, 3D gyro quaternion orientation,
 * touchpad coordinate remapping (1920x1080 -> 1920x942), and Bluetooth force feedback.
 *
 * Multiple controllers: every /dev/bt event already carries the Bluetooth address
 * of the controller that produced it, so the kernel side was never the limit --
 * the reports used to be funnelled into one virtual pad. State is held per device
 * here instead, keyed on that address, so each DualSense drives its own virtual
 * DualShock 4 and so becomes its own player or guest.
 */

#include "bt_driver.h"
#include "daemon_syscalls.h"
#include "vpad.h"
#include "log.h"
#include <stdio.h>
#include <string.h>

// Sony DualSense Identifiers
#define SONY_VID                 0x054C
#define SONY_DUALSENSE_PID       0x0CE6
#define SONY_DUALSENSE_EDGE_PID  0x0DF2

// Syscall Numbers for FreeBSD / Orbis OS
#define SYS_READ      3
#define SYS_WRITE     4
#define SYS_OPEN      5
#define SYS_CLOSE     6
#define SYS_KEXEC     11
#define SYS_IOCTL     54
#define SYS_KQUEUE    362
#define SYS_KEVENT    363

static volatile bool s_bt_running = false;
static int s_bt_fd = -1;
static int s_bt_queue = -1;
static bool s_bt_registered = false;
// How many successful registrations to undo on shutdown.
static int s_bt_reg_count = 0;
static bool s_bt_supported = false;
static uint32_t s_current_fw_bcd = 0x1250;

// -------------------------------------------------------------
// Connected DualSense controllers, one entry each
// -------------------------------------------------------------

typedef struct {
    uint64_t address;         // Bluetooth address; 0 means the entry is free
    int      vpad_slot;       // virtual pad this controller feeds, or -1
    bool     connected;       // reports are arriving from it right now
    uint64_t last_input_time; // for this device's own disconnect timeout
    uint64_t away_since;      // when it went quiet while holding a pad, else 0
    uint32_t reports_seen;    // valid pad reports delivered for this address
    uint32_t decode_drops;    // of those, ones the decoder could not use
    uint8_t  last_drop_len;   // shape of the most recent undecodable report
    uint8_t  last_drop_id;
    uint8_t  rumble_seq;      // output report sequence tag, per device
} BtDevice;

static BtDevice s_devices[BT_MAX_DEVICES];

/* How long a DualSense may stay silent before it counts as away.
 *
 * Upstream used 3 s, chosen when only one DualSense could ever be connected.
 * With two of them the console's Bluetooth link does not sustain both streams
 * evenly: measured on FW 12.52, a controller in active use goes quiet for ~3 s
 * at a stretch and sometimes for tens of seconds, with no decode failures at
 * all (decode_drops stays 0, so the reports are not arriving and being
 * discarded -- they are not arriving). Replacing one DualSense with a
 * DualShock 4 makes the remaining one steady, which is what points at
 * contention on the radio rather than anything in this daemon.
 *
 * So the timeout is a tolerance, not a measurement of death: it has to outlast
 * the gaps the link actually produces. 10 s covers the observed ~3 s stalls
 * with room to spare. */
#define BT_INPUT_TIMEOUT_US 10000000ULL

/* How long a controller's virtual pad is held open after it goes quiet.
 *
 * Deleting the pad the moment a controller drops is what loses the player: the
 * PS4 binds an account to the *device*, so a new pad arrives with no owner and
 * re-opens "Who's using this controller?" -- and whoever answers it can easily
 * land the pad on the wrong account, which is how a guest's controller ends up
 * belonging to the main user. Keeping the pad alive means the same Bluetooth
 * address coming back reattaches to the same pad, account intact, no picker.
 *
 * Generous on purpose. Silences of 20-50 s were measured with two DualSense
 * connected, so a minute of grace still let pads churn, and every churn is
 * another chance for a guest to be reassigned to the wrong account. Ten minutes
 * makes the pad effectively permanent for the length of a session; a controller
 * that is really gone costs one held slot until then, and bt_dev_acquire()
 * reclaims the longest-away entry when a new controller needs one. */
#define BT_PAD_GRACE_US 600000000ULL

/* How many times to register the DualSense VID/PID with the Bluetooth driver.
 *
 * The driver keeps one device entry per successful ioctl 0x8004622B, so N
 * controllers need N registrations. A single registration is what used to cap
 * this daemon at one DualSense: the second controller paired, the system even
 * created an MBus device for it (DEVICE_ADDED type:1 subType:18), but its HID
 * stream was never routed to /dev/bt, so no event ever arrived and no pad was
 * ever created. Measured on FW 12.52: the repeats all return 0, and each one
 * admits one more controller. */
#ifndef ONEFAMILY_BT_REGISTRATIONS
#define ONEFAMILY_BT_REGISTRATIONS BT_MAX_DEVICES
#endif

static void bt_devices_reset(void) {
    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        memset(&s_devices[i], 0, sizeof(s_devices[i]));
        s_devices[i].vpad_slot = -1;
    }
}

static void bt_dev_release(BtDevice *dev);

static BtDevice* bt_dev_find(uint64_t address) {
    if (!address) return NULL;
    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        if (s_devices[i].address == address) return &s_devices[i];
    }
    return NULL;
}

// The entry for this address, claiming a free one if the address is new.
// Returns NULL when every entry is taken.
static BtDevice* bt_dev_acquire(uint64_t address) {
    BtDevice *dev = bt_dev_find(address);
    if (dev) return dev;
    if (!address) return NULL;

    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        if (s_devices[i].address == 0) {
            memset(&s_devices[i], 0, sizeof(s_devices[i]));
            s_devices[i].address = address;
            s_devices[i].vpad_slot = -1;
            s_devices[i].away_since = 0;
            // Start the timeout clock now, so an entry that never produces a
            // decodable report is reclaimed instead of held forever.
            s_devices[i].last_input_time = daemon_sys_time_us();
            daemon_log(LOG_LEVEL_INFO, "DualSense BT: new device 0x%llX (entry %d)",
                       (unsigned long long)address, i);
            return &s_devices[i];
        }
    }

    /* Every entry taken. A long grace window means some of them may be held for
     * controllers that are away, so give up the one that has been away longest
     * rather than turn away a controller that is here and transmitting. */
    BtDevice *oldest = NULL;
    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        if (s_devices[i].connected || !s_devices[i].away_since) continue;
        if (!oldest || s_devices[i].away_since < oldest->away_since) {
            oldest = &s_devices[i];
        }
    }

    if (oldest) {
        daemon_log(LOG_LEVEL_INFO,
                   "DualSense BT: all %d entries in use; reclaiming 0x%llX (Slot %d), away longest, for 0x%llX",
                   BT_MAX_DEVICES, (unsigned long long)oldest->address, oldest->vpad_slot,
                   (unsigned long long)address);
        bt_dev_release(oldest);
        return bt_dev_acquire(address);
    }

    daemon_log(LOG_LEVEL_WARN,
               "DualSense BT: all %d device entries in use and all connected, ignoring address 0x%llX",
               BT_MAX_DEVICES, (unsigned long long)address);
    return NULL;
}

#ifdef ONEFAMILY_BT_DIAG
/* Diagnostics for "the second controller pairs but nothing arrives".
 *
 * The normal path drops any /dev/bt event whose type is outside 0x18..0x20, and
 * any report that fails validation, without a word in the log -- so a log that
 * shows one controller cannot distinguish "no event ever arrived for the second
 * one" from "events arrived and were thrown away". This records both, per
 * Bluetooth address, with hard caps so the log stays readable.
 */
#define BT_DIAG_MAX_ADDR 8
#define BT_DIAG_LOG_PER_ADDR 6

typedef struct {
    uint64_t addr;
    uint32_t events;        // events seen for this address
    uint32_t logged;        // how many we have already written out
    uint32_t accepted;      // reports that passed validation
    uint32_t rejected;      // reports that failed it
    uint8_t  seen_types[8]; // distinct event types, for the summary
    uint32_t n_types;
} BtDiagAddr;

static BtDiagAddr s_diag[BT_DIAG_MAX_ADDR];
static uint64_t s_diag_next_dump = 0;
static uint32_t s_diag_overflow = 0;

static BtDiagAddr* bt_diag_slot(uint64_t addr) {
    for (int i = 0; i < BT_DIAG_MAX_ADDR; ++i) {
        if (s_diag[i].addr == addr) return &s_diag[i];
    }
    for (int i = 0; i < BT_DIAG_MAX_ADDR; ++i) {
        if (s_diag[i].addr == 0) {
            memset(&s_diag[i], 0, sizeof(s_diag[i]));
            s_diag[i].addr = addr;
            daemon_log(LOG_LEVEL_INFO, "[DIAG] first event ever from address 0x%llX (diag entry %d)",
                       (unsigned long long)addr, i);
            return &s_diag[i];
        }
    }
    ++s_diag_overflow;
    return NULL;
}

// Every event off /dev/bt, before the 0x18..0x20 type filter.
static void bt_diag_event(uint8_t ev_type, uint64_t addr, const uint8_t *raw) {
    BtDiagAddr *d = bt_diag_slot(addr);
    if (!d) return;

    ++d->events;

    int known = 0;
    for (uint32_t i = 0; i < d->n_types; ++i) {
        if (d->seen_types[i] == ev_type) { known = 1; break; }
    }
    if (!known && d->n_types < 8) {
        d->seen_types[d->n_types++] = ev_type;
        // A type never seen from this address is always worth a line: a
        // filtered-out type is exactly how a controller goes silent.
        daemon_log(LOG_LEVEL_INFO, "[DIAG] 0x%llX: event type 0x%02X (%s filter 0x18-0x20)",
                   (unsigned long long)addr, ev_type,
                   (ev_type < 0x18 || ev_type > 0x20) ? "OUTSIDE" : "inside");
    }

    if (d->logged < BT_DIAG_LOG_PER_ADDR) {
        ++d->logged;
        // All 16 bytes: the address occupies 8..15, and a 0x18 event carries
        // the PID and VID at 4..7, so the tail is where the identity lives.
        daemon_log(LOG_LEVEL_INFO,
                   "[DIAG] 0x%llX event #%u type=0x%02X raw=%02X%02X%02X%02X%02X%02X%02X%02X"
                   "%02X%02X%02X%02X%02X%02X%02X%02X",
                   (unsigned long long)addr, d->events, ev_type,
                   raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
                   raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]);
    }
}

// The report ioctl's verdict for an event that passed the type filter.
static void bt_diag_report(uint64_t addr, long result, const uint8_t *report, int accepted) {
    BtDiagAddr *d = bt_diag_slot(addr);
    if (!d) return;

    if (accepted) {
        if (d->accepted == 0) {
            daemon_log(LOG_LEVEL_INFO, "[DIAG] 0x%llX: first ACCEPTED report (len=%ld, id=0x%02X)",
                       (unsigned long long)addr, result, report[1]);
        }
        ++d->accepted;
        return;
    }

    ++d->rejected;
    // Only the first few rejections per address; they repeat at 250 Hz.
    if (d->rejected <= BT_DIAG_LOG_PER_ADDR) {
        const char *why = "unknown";
        if (result < 2)                   why = "ioctl returned < 2 (no data / error)";
        else if (result > 80)             why = "length > 80";
        else if (report[0] + 2 != result) why = "report[0]+2 != length";
        else if (report[1] != 0x1F)       why = "report[1] != 0x1F (not a pad report)";
        daemon_log(LOG_LEVEL_WARN, "[DIAG] 0x%llX: REJECTED report #%u result=%ld r0=0x%02X r1=0x%02X -- %s",
                   (unsigned long long)addr, d->rejected, result, report[0], report[1], why);
    }
}

// A standing tally every 5 s, so a controller that goes quiet is visible.
static void bt_diag_dump(void) {
    uint64_t now = daemon_sys_time_us();
    if (now < s_diag_next_dump) return;
    s_diag_next_dump = now + 5000000ULL;

    for (int i = 0; i < BT_DIAG_MAX_ADDR; ++i) {
        BtDiagAddr *d = &s_diag[i];
        if (!d->addr) continue;

        char types[40];
        int o = 0;
        for (uint32_t t = 0; t < d->n_types && o < (int)sizeof(types) - 6; ++t) {
            o += snprintf(types + o, sizeof(types) - (size_t)o, "%02X ", d->seen_types[t]);
        }
        if (o > 0) types[o - 1] = 0; else types[0] = 0;

        BtDevice *bd = bt_dev_find(d->addr);
        daemon_log(LOG_LEVEL_INFO,
                   "[DIAG] tally 0x%llX: events=%u accepted=%u rejected=%u types=[%s] pad_slot=%d",
                   (unsigned long long)d->addr, d->events, d->accepted, d->rejected,
                   types, bd ? bd->vpad_slot : -1);
    }

    if (s_diag_overflow) {
        daemon_log(LOG_LEVEL_WARN, "[DIAG] %u events from more than %d distinct addresses",
                   s_diag_overflow, BT_DIAG_MAX_ADDR);
    }
}
#endif /* ONEFAMILY_BT_DIAG */

/* The controller went quiet but keeps its pad, and so keeps its account.
 * A neutral sample goes out first: a controller that drops mid-press would
 * otherwise leave the pad holding that button down for the whole grace period. */
static void bt_dev_mark_away(BtDevice *dev, uint64_t now) {
    if (!dev) return;

    if (dev->vpad_slot >= 0) {
        OrbisPadData neutral;
        memset(&neutral, 0, sizeof(neutral));
        neutral.leftStick.x = 128;
        neutral.leftStick.y = 128;
        neutral.rightStick.x = 128;
        neutral.rightStick.y = 128;
        neutral.connected = 1;
        neutral.count = 1;
        neutral.timestamp = now;
        vpad_send_data_slot(dev->vpad_slot, &neutral);
    }

    dev->connected = false;
    dev->away_since = now;
}

// Give up one device: close its virtual pad and free the entry.
static void bt_dev_release(BtDevice *dev) {
    if (!dev) return;
    if (dev->vpad_slot >= 0) {
        vpad_slot_close(dev->vpad_slot);
    }
    memset(dev, 0, sizeof(*dev));
    dev->vpad_slot = -1;
}


// -------------------------------------------------------------
// CRC32 Calculation for DualSense Bluetooth Reports
// -------------------------------------------------------------

static inline uint32_t bt_crc_byte(uint32_t crc, uint8_t value) {
    crc ^= value;
    for (unsigned i = 0; i < 8; ++i) {
        crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return crc;
}

static uint32_t bt_calc_crc32(uint8_t prefix, const uint8_t* data, size_t len) {
    uint32_t crc = bt_crc_byte(0xFFFFFFFFU, prefix);
    for (size_t i = 0; i < len; ++i) {
        crc = bt_crc_byte(crc, data[i]);
    }
    return ~crc;
}

// -------------------------------------------------------------
// Kernel Allowlist Patching (Multi-Firmware Hybrid: Verified Table + Safe AOB SigScan)
// -------------------------------------------------------------

struct PatchRequest {
    int restore;
    uint32_t fw_version;             // BCD (e.g. 0x1250, 0x1100, 0x0900, 0x1352)
    int is_dynamic;                  // Output: 0 for static table, 1 for dynamic resolution
    int detail_code;                 // Output: internal diagnostic code
    uint32_t pair_count;             // Output: number of pairs patched (5 or 6)
    uint32_t n_vid_found;
    uint32_t n_pid_found;
    uint32_t discovered_vids[8];
    uint32_t discovered_pids[8];
    uint32_t discovered_offsets[12]; // Output: Discovered offsets for future static tables (up to 6 pairs)
    uint32_t discovered_lock;        // Output: Discovered lock offset
    uint32_t discovered_unlock;      // Output: Discovered unlock offset
};

struct PatchArgs {
    void* function;
    struct PatchRequest* request;
};

// -------------------------------------------------------------
// Dynamic AOB Scanner Cache
// -------------------------------------------------------------
static uint32_t s_dyn_offsets[12];
static uint32_t s_dyn_offsets_count = 0;
static bool     s_dyn_offsets_valid = false;

// Helper: Check if p points to a CMP immediate operand
static inline bool aob_is_cmp_imm(const uint8_t* p) {
    uint8_t b1 = p[-1];
    uint8_t b2 = p[-2];
    uint8_t b3 = p[-3];
    // 3D id (CMP eax, id)
    if (b1 == 0x3D) return true;
    // 81 /7 id (CMP r/m32, id) -> ModR/M byte has reg field == 7 ((b >> 3) & 7 == 7)
    if (b2 == 0x81 && ((b1 >> 3) & 7) == 7) return true;
    if (b3 == 0x81 && ((b2 >> 3) & 7) == 7) return true;
    return false;
}

#define AOB_SAFE_SCAN_START 0x004E0000UL
#define AOB_SAFE_SCAN_END   0x00660000UL

static int aob_find_allowlist_offsets(uint64_t base, uint32_t out_offsets[12], uint32_t* out_count, uint32_t* out_lock, uint32_t* out_unlock, struct PatchRequest* req) {
    uint32_t vids[8];
    uint32_t pids[8];
    int n_vid = 0, n_pid = 0;

    // 1. VID Phase: Scan safe kernel window for CMP imm32 == 0x044F (Thrustmaster VID)
    // 0x044F is exceptionally rare in the kernel and only present in bthid allowlists.
    for (uint64_t a = base + AOB_SAFE_SCAN_START; a + 4 <= base + AOB_SAFE_SCAN_END; ++a) {
        if (*(const volatile uint32_t*)a == 0x0000044FUL) {
            if (aob_is_cmp_imm((const uint8_t*)a)) {
                if (n_vid < 8) vids[n_vid++] = (uint32_t)(a - base);
            }
        }
    }

    if (req) {
        req->n_vid_found = n_vid;
        for (int i = 0; i < n_vid && i < 8; ++i) req->discovered_vids[i] = vids[i];
    }

    // Accept 5 or 6 VIDs (FW 12.50 has 5, FW 13.52 has 6)
    if (n_vid < 5 || n_vid > 6) {
        return 0x310 + n_vid; // Unexpected VID count
    }

    // 2. PID Phase: Search inside the cluster window around the detected VIDs.
    // In Orbis OS bthid, all VIDs and PIDs reside within a compact span.
    uint64_t pid_win_start = (base + vids[0] > 0x800) ? (base + vids[0] - 0x800) : (base + vids[0]);
    uint64_t pid_win_end   = base + vids[n_vid - 1] + 0x8000;
    if (pid_win_end > base + AOB_SAFE_SCAN_END) pid_win_end = base + AOB_SAFE_SCAN_END;

    for (uint64_t a = pid_win_start; a + 4 <= pid_win_end; ++a) {
        if (*(const volatile uint32_t*)a == 0x00001000UL) {
            if (aob_is_cmp_imm((const uint8_t*)a)) {
                if (n_pid < 8) pids[n_pid++] = (uint32_t)(a - base);
            }
        }
    }

    if (req) {
        req->n_pid_found = n_pid;
        for (int i = 0; i < n_pid && i < 8; ++i) req->discovered_pids[i] = pids[i];
    }

    if (n_pid < 5) {
        return 0x320 + n_pid; // Expected at least 5 Thrustmaster PIDs
    }

    // 3. Greedy Nearest-Pairing: assign each VID to its closest unused companion PID
    bool pid_used[8] = {false};
    uint32_t pairs = 0;

    for (int i = 0; i < n_vid && pairs < 6; ++i) {
        uint32_t v = vids[i];
        int best_idx = -1;
        uint32_t best_dist = 0xFFFFFFFFU;

        // Prefer forward PID within 4096 bytes
        for (int j = 0; j < n_pid; ++j) {
            if (pid_used[j]) continue;
            uint32_t p = pids[j];
            if (p > v && (p - v) < 4096) {
                uint32_t dist = p - v;
                if (dist < best_dist) {
                    best_dist = dist;
                    best_idx = j;
                }
            }
        }

        // Fallback: Closest available overall
        if (best_idx < 0) {
            for (int j = 0; j < n_pid; ++j) {
                if (pid_used[j]) continue;
                uint32_t p = pids[j];
                uint32_t dist = (p > v) ? (p - v) : (v - p);
                if (dist < best_dist) {
                    best_dist = dist;
                    best_idx = j;
                }
            }
        }

        if (best_idx >= 0) {
            pid_used[best_idx] = true;
            out_offsets[2 * pairs]     = v;
            out_offsets[2 * pairs + 1] = pids[best_idx];
            ++pairs;
        }
    }

    if (pairs < 5) {
        return 0x330 + pairs;
    }

    *out_count = pairs;

    // 4. Resolve lock / unlock mutexes relative to the first VID anchor (12.50 delta = -0xA226 and -0xA1A6)
    int64_t delta = (int64_t)((base + vids[0]) - (base + 0x5B0DA6));
    uint64_t lock_cand   = (base + 0x5A6B80) + delta;
    uint64_t unlock_cand = (base + 0x5A6C00) + delta;

    if ((*(const volatile uint32_t*)lock_cand & 0x00FFFFFF) == 0x00E58948 ||
        *(const volatile uint8_t*)lock_cand == 0x55) {
        *out_lock   = (uint32_t)(lock_cand - base);
        *out_unlock = (uint32_t)(unlock_cand - base);
    } else {
        *out_lock   = 0;
        *out_unlock = 0;
    }

    return 0;
}

/**
 * Ring 0 Kernel Execution callback (invoked via syscall 11 / kexec)
 * Hybrid Multi-Firmware: Verified Table (12.50) + Safe Contextual AOB Scanner (Other FW)
 */
static int patch_allowlist(void* thread, struct PatchArgs* args) {
    (void)thread;
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000082));
    uint64_t base = (((uint64_t)high << 32) | low) - 0x1C0;

    if ((base >> 48) != 0xFFFF || (base & 0x3FFF) || !args || !args->request) {
        return 22; // EINVAL
    }

    struct PatchRequest* req = args->request;
    int restore = req->restore;
    if (restore != 0 && restore != 1) return 22;

    uint32_t lock_off = 0;
    uint32_t unlock_off = 0;
    uint32_t active_offsets[12];
    uint32_t total_offsets = 10;

    // Path 1: Verified Static Table (FW 12.50)
    if (req->fw_version == 0x1250) {
        if (*(volatile uint64_t*)(base + 0x5B6D00) != 0x56415741E5894855UL) {
            req->detail_code = 0x1250;
            return 22;
        }
        lock_off = 0x5A6B80;
        unlock_off = 0x5A6C00;
        const uint32_t static_1250[10] = {
            0x5B0DA6, 0x5B0DD6, 0x5B1616, 0x5B1649, 0x5B53D8,
            0x5B5FCA, 0x5B578D, 0x5B5C6E, 0x5B580B, 0x5B5CFE
        };
        for (int i = 0; i < 10; ++i) active_offsets[i] = static_1250[i];
        total_offsets = 10;
        req->is_dynamic = 0;
        req->pair_count = 5;
    }
    // Path 2: Verified Static Table (FW 13.52)
    else if (req->fw_version == 0x1352) {
        if (*(volatile uint32_t*)(base + 0x5B1616) != 0x0000044FUL) {
            req->detail_code = 0x1352;
            return 22;
        }
        lock_off = 0;
        unlock_off = 0;
        const uint32_t static_1352[12] = {
            0x59C869, 0x59C8C0,
            0x5B1616, 0x5B1646,
            0x5B1E86, 0x5B1EB9,
            0x5B5C48, 0x5B64DE,
            0x5B5FFD, 0x5B656E,
            0x5B607B, 0x5B348A
        };
        for (int i = 0; i < 12; ++i) active_offsets[i] = static_1352[i];
        total_offsets = 12;
        req->is_dynamic = 0;
        req->pair_count = 6;
    }
    // Path 3: Safe Contextual AOB Scanner for unlisted firmwares (11.00, 9.00, 10.xx, etc.)
    else {
        // Path 2: Safe Contextual AOB Scanner for unlisted firmwares (13.52, 9.00, etc.)
        if (restore && s_dyn_offsets_valid) {
            total_offsets = s_dyn_offsets_count;
            for (uint32_t i = 0; i < total_offsets; ++i) active_offsets[i] = s_dyn_offsets[i];
            req->is_dynamic = 1;
            req->pair_count = total_offsets / 2;
        } else {
            uint32_t pair_count = 0;
            int aob_rc = aob_find_allowlist_offsets(base, active_offsets, &pair_count, &lock_off, &unlock_off, req);
            req->detail_code = aob_rc;
            if (aob_rc != 0) {
                return 45; // Safe abort: no memory touched, fallback cleanly to USB
            }
            total_offsets = pair_count * 2;
            for (uint32_t i = 0; i < total_offsets; ++i) s_dyn_offsets[i] = active_offsets[i];
            s_dyn_offsets_count = total_offsets;
            s_dyn_offsets_valid = true;
            req->is_dynamic = 1;
            req->pair_count = pair_count;
        }
    }

    for (uint32_t i = 0; i < total_offsets; ++i) req->discovered_offsets[i] = active_offsets[i];
    req->discovered_lock = lock_off;
    req->discovered_unlock = unlock_off;

    int (*lock)(void) = lock_off ? (void*)(base + lock_off) : NULL;
    int (*unlock)(int) = unlock_off ? (void*)(base + unlock_off) : NULL;

    if (lock) {
        int rc = lock();
        if (rc) return rc;
    }

    unsigned original_count = 0, patched_count = 0;
    for (unsigned i = 0; i < total_offsets; ++i) {
        uint32_t old_value = (i & 1) ? 0x1000 : 0x044F;
        uint32_t new_value = (i & 1) ? 0x0CE6 : 0x054C;
        uint32_t current = *(volatile uint32_t*)(base + active_offsets[i]);
        if (current == old_value) ++original_count;
        else if (current == new_value) ++patched_count;
        else {
            if (unlock) unlock(0);
            return 0x100 + (int)i;
        }
    }

    if (original_count && patched_count) {
        if (unlock) unlock(0);
        return 0x200 + (int)original_count;
    }

    if ((!restore && patched_count == total_offsets) || (restore && original_count == total_offsets)) {
        if (unlock) unlock(0);
        return 0;
    }

    uint64_t flags, cr0;
    __asm__ volatile("pushfq; popq %0; cli; mov %%cr0, %1" : "=r"(flags), "=r"(cr0) :: "memory");
    uint64_t writable = cr0 & ~(1UL << 16);
    __asm__ volatile("mov %0, %%cr0" :: "r"(writable) : "memory");

    for (unsigned i = 0; i < total_offsets; ++i) {
        uint32_t old_value = (i & 1) ? 0x1000 : 0x044F;
        uint32_t new_value = (i & 1) ? 0x0CE6 : 0x054C;
        *(volatile uint32_t*)(base + active_offsets[i]) = restore ? old_value : new_value;
    }

    __asm__ volatile("mov %0, %%cr0; pushq %1; popfq" :: "r"(cr0), "r"(flags) : "memory", "cc");
    if (unlock) unlock(0);
    return 0;
}

static int apply_kernel_allowlist_patch(int restore, uint32_t fw_version) {
    struct PatchRequest req;
    memset(&req, 0, sizeof(req));
    req.restore = restore;
    req.fw_version = fw_version;

    long ret = daemon_syscall(SYS_KEXEC, (long)patch_allowlist, (long)&req, 0);
    daemon_log(LOG_LEVEL_INFO, "DualSense BT Kernel Allowlist Patch (restore=%d, fw=0x%04X, dynamic=%d, detail=0x%X) = %ld",
               restore, fw_version, req.is_dynamic, req.detail_code, ret);

    if (req.n_vid_found > 0) {
        daemon_log(LOG_LEVEL_INFO, "[FW 0x%04X] AOB Scan detected %u VIDs and %u PIDs:", fw_version, req.n_vid_found, req.n_pid_found);
        for (uint32_t i = 0; i < req.n_vid_found && i < 8; ++i) {
            daemon_log(LOG_LEVEL_INFO, "  VID[%u] = 0x%06X", i, req.discovered_vids[i]);
        }
        for (uint32_t i = 0; i < req.n_pid_found && i < 8; ++i) {
            daemon_log(LOG_LEVEL_INFO, "  PID[%u] = 0x%06X", i, req.discovered_pids[i]);
        }
    }

    if (ret == 0 && req.is_dynamic && !restore) {
        daemon_log(LOG_LEVEL_INFO, "[FW 0x%04X] Successfully paired and patched %u Pairs (%u Offsets) for DualSense Wireless Pairing:",
                   fw_version, req.pair_count, req.pair_count * 2);
        for (uint32_t i = 0; i < req.pair_count; ++i) {
            daemon_log(LOG_LEVEL_INFO, "  Pair[%u]: VID=0x%06X, PID=0x%06X",
                       i, req.discovered_offsets[2 * i], req.discovered_offsets[2 * i + 1]);
        }
        daemon_log(LOG_LEVEL_INFO, "  Lock/Unlock: lock=0x%06X, unlock=0x%06X",
                   req.discovered_lock, req.discovered_unlock);
    }
    return (int)ret;
}

// -------------------------------------------------------------
// Bluetooth Report 0x31 / 0x01 Decoder
// -------------------------------------------------------------

static void decode_dualsense_bt_report(BtDevice* dev, const uint8_t* report, size_t length) {
    static const uint32_t dpad_map[9] = {
        DS4_BUTTON_DPAD_UP,
        DS4_BUTTON_DPAD_UP | DS4_BUTTON_DPAD_RIGHT,
        DS4_BUTTON_DPAD_RIGHT,
        DS4_BUTTON_DPAD_DOWN | DS4_BUTTON_DPAD_RIGHT,
        DS4_BUTTON_DPAD_DOWN,
        DS4_BUTTON_DPAD_DOWN | DS4_BUTTON_DPAD_LEFT,
        DS4_BUTTON_DPAD_LEFT,
        DS4_BUTTON_DPAD_UP | DS4_BUTTON_DPAD_LEFT,
        0
    };

    if (!dev || !report) return;

    uint8_t simple[10] = {0};
    const uint8_t* data = NULL;

    dev->last_drop_len = (uint8_t)(length > 255 ? 255 : length);
    dev->last_drop_id = report[0];

    if (length == 10 && report[0] == 0x01) {
        // Remap 10-byte simple report to match 78-byte payload indices (Diram1x logic)
        for (unsigned i = 0; i < 4; ++i) simple[i] = report[i + 1];
        simple[4] = report[8];       // L2 trigger
        simple[5] = report[9];       // R2 trigger
        simple[7] = report[5];       // D-Pad + Face buttons
        simple[8] = report[6];       // Bumpers, triggers, options, L3, R3
        simple[9] = report[7] & 3;   // PS and touchpad buttons
        data = simple;
    } else if (length == 78 && report[0] == 0x31) {
        // Extended 78-byte Bluetooth report with CRC32
        uint32_t expected_crc = (uint32_t)report[74] |
                                ((uint32_t)report[75] << 8) |
                                ((uint32_t)report[76] << 16) |
                                ((uint32_t)report[77] << 24);
        uint32_t computed_crc = bt_calc_crc32(0xA1, report, 74);
        if (computed_crc != expected_crc) {
            ++dev->decode_drops; // CRC mismatch: corrupted Bluetooth packet
            return;
        }
        data = report + 2; // Data payload begins at offset 2
    } else {
        ++dev->decode_drops;
        return;
    }

    if (!data) {
        ++dev->decode_drops;
        return;
    }

    unsigned direction = data[7] & 0x0F;
    if (direction > 8) {
        ++dev->decode_drops;
        return;
    }

    if (!dev->connected) {
        if (dev->vpad_slot >= 0) {
            /* It still holds the pad it had before it went quiet, so it comes
             * straight back to the same player and the same account. No new
             * device, no user picker, nothing to answer wrongly. */
            uint64_t away_us = dev->away_since ? (daemon_sys_time_us() - dev->away_since) : 0;
            dev->connected = true;
            dev->away_since = 0;

            int player = vpad_get_assigned_slot_num(dev->vpad_slot);
            daemon_log(LOG_LEVEL_INFO,
                       "controller_returned: DualSense BT 0x%llX kept Slot %d (Player %d) after %llu ms away "
                       "[reports=%u decode_drops=%u]",
                       (unsigned long long)dev->address, dev->vpad_slot, player,
                       (unsigned long long)(away_us / 1000),
                       dev->reports_seen, dev->decode_drops);
            daemon_notify("OneFamily: DualSense (Player %d) Reconnected", player);
        } else {
            // Keyed on this controller's address, so a second DualSense opens a
            // second virtual pad rather than reusing the first one's.
            if (vpad_slot_open_dev(VPAD_SOURCE_BT, dev->address, &dev->vpad_slot) != 0 ||
                dev->vpad_slot < 0) {
                // No virtual pad to be had. Drop the report rather than feed it
                // to somebody else's pad; the next report retries.
                dev->vpad_slot = -1;
                daemon_log(LOG_LEVEL_WARN, "DualSense BT 0x%llX: no virtual pad slot available",
                           (unsigned long long)dev->address);
                dev->last_input_time = daemon_sys_time_us();
                return;
            }
            dev->connected = true;
            dev->away_since = 0;

            int player = vpad_get_assigned_slot_num(dev->vpad_slot);
            daemon_notify("OneFamily: DualSense Wireless (Player %d)", player);
            daemon_log(LOG_LEVEL_INFO, "controller_connected: DualSense BT 0x%llX (Slot %d, Player %d)",
                       (unsigned long long)dev->address, dev->vpad_slot, player);
        }
    }

    dev->last_input_time = daemon_sys_time_us();

    OrbisPadData ds4;
    memset(&ds4, 0, sizeof(ds4));

    // Analog Sticks
    ds4.leftStick.x  = data[0];
    ds4.leftStick.y  = data[1];
    ds4.rightStick.x = data[2];
    ds4.rightStick.y = data[3];

    // Analog Triggers
    ds4.analogButtons.l2 = data[4];
    ds4.analogButtons.r2 = data[5];

    // D-Pad
    ds4.buttons = dpad_map[direction];

    // Face Buttons
    if (data[7] & 0x10) ds4.buttons |= DS4_BUTTON_SQUARE;
    if (data[7] & 0x20) ds4.buttons |= DS4_BUTTON_CROSS;
    if (data[7] & 0x40) ds4.buttons |= DS4_BUTTON_CIRCLE;
    if (data[7] & 0x80) ds4.buttons |= DS4_BUTTON_TRIANGLE;

    // Bumpers, Triggers & Stick Clicks
    if (data[8] & 0x01) ds4.buttons |= DS4_BUTTON_L1;
    if (data[8] & 0x02) ds4.buttons |= DS4_BUTTON_R1;
    if (data[8] & 0x04) ds4.buttons |= DS4_BUTTON_L2;
    if (data[8] & 0x08) ds4.buttons |= DS4_BUTTON_R2;
    if (data[8] & 0x10) ds4.buttons |= DS4_BUTTON_SHARE;   // Create
    if (data[8] & 0x20) ds4.buttons |= DS4_BUTTON_OPTIONS; // Options
    if (data[8] & 0x40) ds4.buttons |= DS4_BUTTON_L3;
    if (data[8] & 0x80) ds4.buttons |= DS4_BUTTON_R3;

    // System Buttons
    if (data[9] & 0x01) ds4.buttons |= DS4_BUTTON_PS;
    if (data[9] & 0x02) ds4.buttons |= DS4_BUTTON_TOUCHPAD;

    // Quick Combo: Create + Options -> Touchpad Click
    if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS)) == (DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS)) {
        ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS);
        ds4.buttons |= DS4_BUTTON_TOUCHPAD;
    }

    // Quick Combo: Create + D-Pad Down -> PS Button
    if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) == (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) {
        ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN);
        ds4.buttons |= DS4_BUTTON_PS;
    }

    ds4.connected = 1;
    ds4.timestamp = dev->last_input_time;

    // Send the translated packet to this controller's own Virtual DualShock 4.
    // No fallback to vpad_send_data(): with several pads open that would pick an
    // arbitrary slot and move the wrong player.
    if (dev->vpad_slot >= 0) {
        vpad_send_data_slot(dev->vpad_slot, &ds4);
    }
}

// -------------------------------------------------------------
// Non-Blocking Polling Step (250 Hz Daemon Loop)
// -------------------------------------------------------------

/* One registration with the Bluetooth driver.
 *
 * ioctl 0x8004622B tells the driver to hand devices matching this VID/PID to our
 * /dev/bt descriptor. Upstream calls it once. Whether the driver keeps one entry
 * per pair -- which would leave a second identical controller with nowhere to
 * land -- is what ONEFAMILY_BT_EXTRA_REG exists to find out, so every call's
 * return value is logged rather than discarded.
 */
static long bt_register_pair(uint16_t vid, uint16_t pid, const char *what) {
    struct {
        uint16_t vendor;
        uint16_t product;
    } device = { vid, pid };
    long rc = daemon_syscall(SYS_IOCTL, s_bt_fd, 0x8004622BUL, (long)&device);
    daemon_log(LOG_LEVEL_INFO, "register %s (%04X:%04X) = %ld", what, vid, pid, rc);
    return rc;
}

static long bt_unregister_pair(uint16_t vid, uint16_t pid) {
    struct {
        uint16_t vendor;
        uint16_t product;
    } device = { vid, pid };
    long rc = daemon_syscall(SYS_IOCTL, s_bt_fd, 0x8004622CUL, (long)&device);
    daemon_log(LOG_LEVEL_INFO, "unregister (%04X:%04X) = %ld", vid, pid, rc);
    return rc;
}

// Each controller times out on its own clock: one player setting a pad down
// must not drop anybody else. Runs on every poll step, including steps that do
// have an event to read, so a silent pad still expires while the others play.
static void bt_check_timeouts(void) {
    uint64_t now = daemon_sys_time_us();

    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        BtDevice *dev = &s_devices[i];
        if (!dev->address) continue;

        // Already away: its pad is being held for it. Release it only once the
        // grace window has run out.
        if (!dev->connected && dev->vpad_slot >= 0) {
            if (now - dev->away_since <= BT_PAD_GRACE_US) continue;

            int player = vpad_get_assigned_slot_num(dev->vpad_slot);
            daemon_log(LOG_LEVEL_INFO,
                       "controller_disconnected: DualSense BT 0x%llX gone past the grace window, releasing Slot %d (Player %d)",
                       (unsigned long long)dev->address, dev->vpad_slot, player);
            bt_dev_release(dev);
            daemon_notify("OneFamily: DualSense (Player %d) Disconnected", player);
            continue;
        }

        if (now - dev->last_input_time <= BT_INPUT_TIMEOUT_US) continue;

        if (!dev->connected) {
            // Claimed an entry but never produced a decodable report, so it
            // holds no pad: free it quietly for a real controller.
            daemon_log(LOG_LEVEL_INFO, "DualSense BT: dropping silent device 0x%llX (entry %d)",
                       (unsigned long long)dev->address, i);
            bt_dev_release(dev);
            continue;
        }

        // Just went quiet. Keep the pad, and with it the account, in case this
        // is a controller switching off or stepping out of range.
        int player = dev->vpad_slot >= 0 ? vpad_get_assigned_slot_num(dev->vpad_slot) : 0;
        daemon_log(LOG_LEVEL_INFO,
                   "controller_away: DualSense BT 0x%llX quiet %llu ms, holding Slot %d (Player %d) for %llu s "
                   "[reports=%u decode_drops=%u last_drop len=%u id=0x%02X]",
                   (unsigned long long)dev->address,
                   (unsigned long long)((now - dev->last_input_time) / 1000ULL),
                   dev->vpad_slot, player,
                   (unsigned long long)(BT_PAD_GRACE_US / 1000000ULL),
                   dev->reports_seen, dev->decode_drops,
                   dev->last_drop_len, dev->last_drop_id);
        bt_dev_mark_away(dev, now);
    }
}

void bt_driver_poll_step(void) {
    if (!s_bt_running || s_bt_fd < 0 || s_bt_queue < 0) return;

    struct {
        long seconds;
        long nanoseconds;
    } timeout = { 0, 0 };

    struct {
        uint64_t ident;
        short    filter;
        uint16_t flags;
        uint32_t fflags;
        long     data;
        void*    udata;
    } event;

    long ready = daemon_sys_call6(SYS_KEVENT, s_bt_queue, 0, 0, (long)&event, 1, (long)&timeout);

    bt_check_timeouts();

#ifdef ONEFAMILY_BT_DIAG
    bt_diag_dump();
#endif

    if (ready <= 0) {
        return;
    }

    uint8_t buffer[256];
    long count = daemon_sys_read(s_bt_fd, buffer, sizeof(buffer));
    if (count > 0 && count <= (long)sizeof(buffer)) {
        for (long pos = 0; pos + 16 <= count; pos += 16) {
            uint8_t ev_type = buffer[pos];

            // The address is read before the type filter: an event of a type we
            // ignore still says which controller it came from, which is what a
            // diagnostic build needs in order to account for every event.
            uint64_t address = 0;
            for (unsigned i = 0; i < 8; ++i) {
                address |= ((uint64_t)buffer[pos + 8 + i]) << (8 * i);
            }

#ifdef ONEFAMILY_BT_DIAG
            bt_diag_event(ev_type, address, &buffer[pos]);
#endif

            if (ev_type < 0x18 || ev_type > 0x20) continue;

            uint8_t report[80];
            memset(report, 0, sizeof(report));
            struct {
                uint64_t address;
                void*    buffer;
                uint64_t capacity;
                uint64_t reserved;
            } req = { address, report, sizeof(report), 0 };

            long result = daemon_syscall(SYS_IOCTL, s_bt_fd, 0x80206233UL, (long)&req);
            int ok = (result >= 2 && result <= 80 && report[0] + 2 == result && report[1] == 0x1F);
#ifdef ONEFAMILY_BT_DIAG
            bt_diag_report(address, result, report, ok);
#endif
            if (ok) {
                // The address came from the event, so the report goes to that
                // controller's entry -- never to a single shared one.
                BtDevice *dev = bt_dev_acquire(address);
                if (dev) {
                    /* A valid pad report for this address means the controller
                     * is alive, whatever the decoder then makes of its contents.
                     * Tying liveness to a successful decode instead conflated
                     * "alive" with "a report shape we understand", so a run of
                     * reports the decoder skips -- an unexpected length, a bad
                     * CRC, a reserved D-pad value -- used to expire a controller
                     * that was transmitting perfectly well, destroying its pad
                     * and its account along with it. */
                    ++dev->reports_seen;
                    dev->last_input_time = daemon_sys_time_us();

                    decode_dualsense_bt_report(dev, report + 2, (size_t)(result - 2));
                }
            }
        }
    }
}

// -------------------------------------------------------------
// Public Subsystem API
// -------------------------------------------------------------

int bt_driver_init(void) {
    daemon_log(LOG_LEVEL_INFO, "Initializing DualSense Bluetooth Subsystem (up to %d controllers)...",
               BT_MAX_DEVICES);
    bt_devices_reset();

    // 1. Verify firmware version (sceKernelGetSystemSwVersion)
    struct {
        uint64_t size;
        char     text[28];
        uint32_t version;
    } sw_info = { .size = 40 };

    int (*get_sw_version)(void*) = NULL;
    int hKernel = daemon_kernel_get_handle();
    if (hKernel != 0) {
        daemon_sys_dlsym(hKernel, "sceKernelGetSystemSwVersion", (void**)&get_sw_version);
    }
    if (!get_sw_version) {
        daemon_syscall(591, 0x2001, (long)"sceKernelGetSystemSwVersion", (long)&get_sw_version);
    }

    if (get_sw_version) {
        int v_rc = get_sw_version(&sw_info);
        daemon_log(LOG_LEVEL_INFO, "resolve_version: rc=%d, firmware=0x%08X (%s)",
                   v_rc, sw_info.version, sw_info.text);
        if (v_rc == 0) {
            s_current_fw_bcd = (sw_info.version >> 16);
        }
    } else {
        daemon_log(LOG_LEVEL_WARN, "DualSense BT: sceKernelGetSystemSwVersion unresolved, defaulting to FW 0x1250 / dynamic.");
    }

    // 2. Open /dev/bt device node
    s_bt_fd = daemon_sys_open("/dev/bt", 2 /* O_RDWR */, 0);
    daemon_log(LOG_LEVEL_INFO, "open_hid (/dev/bt) = %d", s_bt_fd);
    if (s_bt_fd < 0) {
        daemon_log(LOG_LEVEL_WARN, "Failed to open /dev/bt (fd=%d)", s_bt_fd);
        return -2;
    }

    // 3. Create kqueue
    s_bt_queue = daemon_syscall(SYS_KQUEUE, 0, 0, 0);
    daemon_log(LOG_LEVEL_INFO, "kqueue = %d", s_bt_queue);
    if (s_bt_queue < 0) {
        daemon_sys_close(s_bt_fd);
        s_bt_fd = -1;
        return -3;
    }

    // 4. Watch EVFILT_READ on s_bt_fd via kevent
    struct {
        uint64_t ident;
        short    filter; // EVFILT_READ = -1
        uint16_t flags;  // EV_ADD = 1
        uint32_t fflags;
        long     data;
        void*    udata;
    } change = {
        (uint64_t)s_bt_fd, -1, 1, 0, 0, NULL
    };
    long kevent_rc = daemon_sys_call6(SYS_KEVENT, s_bt_queue, (long)&change, 1, 0, 0, 0);
    daemon_log(LOG_LEVEL_INFO, "watch_read = %ld", kevent_rc);
    if (kevent_rc != 0) {
        daemon_sys_close(s_bt_queue);
        s_bt_queue = -1;
        daemon_sys_close(s_bt_fd);
        s_bt_fd = -1;
        return -4;
    }

    // 5. Subscribe to Bluetooth controller read stream with mask
    uint64_t mask = 0x1FF000000UL;
    long sub_rc = daemon_sys_write(s_bt_fd, &mask, sizeof(mask));
    daemon_log(LOG_LEVEL_INFO, "subscribe = %ld (expected 8)", sub_rc);
    if (sub_rc != (long)sizeof(mask)) {
        daemon_sys_close(s_bt_queue);
        s_bt_queue = -1;
        daemon_sys_close(s_bt_fd);
        s_bt_fd = -1;
        return -5;
    }

    // 6. Apply temporary kernel allowlist patch (Ring 0)
    int patch_rc = apply_kernel_allowlist_patch(0 /* patch */, s_current_fw_bcd);
    if (patch_rc != 0) {
        daemon_log(LOG_LEVEL_WARN, "DualSense BT: Kernel allowlist patch failed or FW 0x%04X not supported (rc=%d)",
                   s_current_fw_bcd, patch_rc);
        daemon_sys_close(s_bt_queue);
        s_bt_queue = -1;
        daemon_sys_close(s_bt_fd);
        s_bt_fd = -1;
        s_bt_supported = false;
        return -6;
    }

    // 7. Register DualSense device with Bluetooth driver (ioctl 0x8004622B)
    long reg_rc = bt_register_pair(SONY_VID, SONY_DUALSENSE_PID, "dualsense");
    if (reg_rc != 0) {
        // Registration failed: restore allowlist, close descriptors, return cleanly
        apply_kernel_allowlist_patch(1 /* restore */, s_current_fw_bcd);
        daemon_sys_close(s_bt_queue);
        s_bt_queue = -1;
        daemon_sys_close(s_bt_fd);
        s_bt_fd = -1;
        s_bt_supported = false;
        return -7;
    }

    s_bt_registered = true;
    s_bt_reg_count = 1;

    /* One more registration per additional controller. Best effort: the first
     * registration above is the one that gates the subsystem, and a failure
     * here only costs a player, never the controller that already works. */
    for (int i = 1; i < ONEFAMILY_BT_REGISTRATIONS; ++i) {
        if (bt_register_pair(SONY_VID, SONY_DUALSENSE_PID, "dualsense (slot)") == 0) {
            ++s_bt_reg_count;
        }
    }

#ifdef ONEFAMILY_BT_REGISTER_EDGE
    /* The Edge's own PID is refused on FW 12.52, so this is off by default
     * rather than printing an error into everybody's log on every boot. */
    bt_register_pair(SONY_VID, SONY_DUALSENSE_EDGE_PID, "dualsense edge");
#endif

    daemon_log(LOG_LEVEL_INFO,
               "DualSense BT: %d of %d registrations held -> room for %d simultaneous controllers",
               s_bt_reg_count, ONEFAMILY_BT_REGISTRATIONS, s_bt_reg_count);

    s_bt_running = true;
    s_bt_supported = true;
    daemon_log(LOG_LEVEL_INFO, "DualSense Bluetooth Subsystem initialized successfully (FW 0x%04X)", s_current_fw_bcd);
    return 0;
}

void bt_driver_shutdown(void) {
    if (!s_bt_running && s_bt_fd < 0 && !s_bt_registered) return;

    daemon_log(LOG_LEVEL_INFO, "Shutting down DualSense Bluetooth Subsystem...");
    s_bt_running = false;
    s_bt_supported = false;

    if (s_bt_registered && s_bt_fd >= 0) {
        // Give back exactly as many registrations as were taken.
        for (int i = 0; i < s_bt_reg_count; ++i) {
            bt_unregister_pair(SONY_VID, SONY_DUALSENSE_PID);
        }
#ifdef ONEFAMILY_BT_REGISTER_EDGE
        bt_unregister_pair(SONY_VID, SONY_DUALSENSE_EDGE_PID);
#endif
        s_bt_reg_count = 0;
        s_bt_registered = false;
    }

    apply_kernel_allowlist_patch(1 /* restore */, s_current_fw_bcd);

    if (s_bt_queue >= 0) {
        daemon_sys_close(s_bt_queue);
        s_bt_queue = -1;
    }

    if (s_bt_fd >= 0) {
        daemon_sys_close(s_bt_fd);
        s_bt_fd = -1;
    }

    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        if (s_devices[i].address) {
            bt_dev_release(&s_devices[i]);
        }
    }
    bt_devices_reset();
}

bool bt_driver_is_supported(void) {
    return s_bt_supported;
}

bool bt_driver_is_connected(void) {
    return bt_driver_connected_count() > 0;
}

int bt_driver_connected_count(void) {
    int n = 0;
    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        if (s_devices[i].connected) ++n;
    }
    return n;
}

static void bt_send_rumble_dev(BtDevice *dev, uint8_t large, uint8_t small, const uint8_t color[3]) {
    if (!s_bt_running || s_bt_fd < 0 || !dev || !dev->connected || !dev->address) return;

    uint8_t out[78];
    memset(out, 0, sizeof(out));

    out[0] = 0x31;                  // Report ID
    out[1] = (dev->rumble_seq++ & 0x0F) << 4;   // Sequence tag, per controller
    out[2] = 0x10;
    out[3] = 0x03;                  // Enable motors
    out[4] = 0x14;
    out[5] = small;                 // Weak motor
    out[6] = large;                 // Strong motor
    out[46] = 0x04;                 // Enable lightbar color

    if (color) {
        out[47] = color[0];
        out[48] = color[1];
        out[49] = color[2];
    } else {
        out[47] = 0;
        out[48] = 112;
        out[49] = 209; // Default PlayStation Blue
    }

    // Compute CRC32 checksum for Bluetooth output packet
    uint32_t crc = bt_calc_crc32(0xA2, out, 74);
    for (unsigned i = 0; i < 4; ++i) {
        out[74 + i] = (uint8_t)(crc >> (8 * i));
    }

    // Send Bluetooth output request via ioctl, addressed to this controller
    struct {
        uint64_t address;
        void* buffer;
        uint64_t size;
        uint64_t report_id;
    } req = { dev->address, out, 78, 0x31 };

    daemon_syscall(SYS_IOCTL, s_bt_fd, 0x80206232UL, (long)&req);
}

void bt_driver_send_rumble_slot(int vpad_slot, uint8_t large, uint8_t small, const uint8_t color[3]) {
    if (vpad_slot < 0) return;

    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        if (s_devices[i].connected && s_devices[i].vpad_slot == vpad_slot) {
            bt_send_rumble_dev(&s_devices[i], large, small, color);
            return;
        }
    }
}

void bt_driver_send_rumble(uint8_t large, uint8_t small, const uint8_t color[3]) {
    // No slot given: the first connected controller is the only sensible target.
    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        if (s_devices[i].connected) {
            bt_send_rumble_dev(&s_devices[i], large, small, color);
            return;
        }
    }
}
