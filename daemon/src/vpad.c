/*
 * Virtual DualShock 4 Device Handler for OneFamily Daemon
 * Creates native Virtual DualShock 4 (Device Type 1) via libScePad
 * Compatible with XMB, PS4 games, homebrew, and system overlays
 * Supports Multi-User & Player Slot binding via libSceUserService & libSceMbus
 */

#include "vpad.h"
#include "bt_driver.h"
#include "usb_driver.h"
#include "config.h"
#include "log.h"
#include "daemon_syscalls.h"
#include "ds4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* AddDevice's parameter block.
 *
 * 16 bytes, as upstream has it. Control4Free declares 32, and declaring 32 here
 * was tried on FW 12.52: the call accepts it and returns the same handles, so
 * the extra words change nothing and the smaller struct stays.
 *
 * The word at offset 4 is `userId`, not `type`: Control4Free names it that and
 * documents that the value 1 means "arrives with no user, so the PS4 opens its
 * 'Who's using this controller?' screen", which is exactly what both projects
 * pass and exactly what both see. */
typedef struct {
    uint32_t size;
    uint32_t userId;      // 1 = unassigned, let the PS4 ask who is using it
    uint32_t flags;
    uint32_t reserved;
} ScePadVirtualDeviceParam;

// libScePad APIs
typedef int (*scePadInit_t)(void);
typedef int (*scePadMbusInit_t)(void);
typedef int (*scePadSetProcessPrivilege_t)(int);
typedef int (*scePadSetConnection_t)(int handle, int connection);
typedef int (*scePadVirtualDeviceAddDevice_t)(const ScePadVirtualDeviceParam *param, int *out_handle);
typedef int (*scePadVirtualDeviceInsertData_t)(int handle, const void *data);
typedef int (*scePadVirtualDeviceDeleteDevice_t)(int handle);
typedef int (*scePadVirtualDeviceGetRemoteSetting_t)(int handle, void *out_setting);
typedef int32_t (*scePadGetHandle_t)(int32_t userId, uint32_t type, uint32_t index);
typedef int32_t (*scePadOpen_t)(int32_t userId, int32_t type, int32_t index, void *param);
typedef int32_t (*scePadClose_t)(int32_t handle);
typedef int32_t (*scePadGetDeviceId_t)(int32_t handle, int64_t *outDeviceId);
typedef int32_t (*scePadDisconnectPort_t)(int32_t handle);
typedef int32_t (*scePadDisconnectDevice_t)(int32_t handle_or_devId);

// libSceMbus APIs
typedef int (*sceMbusInit_t)(void);
typedef int (*sceMbusBindDeviceWithUserId_t)(int deviceId, int32_t userId);
typedef int (*sceMbusDisconnectDevice_t)(int deviceId);
typedef int32_t (*sceMbusResolveByHandle_t)(int32_t handle, int64_t *outDeviceId);

// libSceHidControl APIs
typedef int32_t (*sceHidControlInit_t)(void);
typedef int32_t (*sceHidControlGetDeviceId_t)(int32_t handle, int64_t *outDeviceId);
typedef int32_t (*sceHidControlDisconnectPort_t)(int32_t handle);
typedef int32_t (*sceHidControlDisconnectDevice_t)(int32_t deviceId);
typedef int32_t (*sceHidControlDisconnectHandle_t)(int32_t handle);

// libSceUserService APIs
#define SCE_USER_SERVICE_MAX_LOGIN_USERS 4
#define SCE_USER_SERVICE_USER_ID_INVALID (-1)

typedef int32_t SceUserServiceUserId;

typedef struct {
    SceUserServiceUserId userId[SCE_USER_SERVICE_MAX_LOGIN_USERS];
} SceUserServiceLoginUserIdList;

typedef int (*sceUserServiceInitialize_t)(void *params);
typedef int (*sceUserServiceGetInitialUser_t)(SceUserServiceUserId *userId);
typedef int (*sceUserServiceGetLoginUserIdList_t)(SceUserServiceLoginUserIdList *userIdList);
typedef int (*sceUserServiceGetUserName_t)(SceUserServiceUserId userId, char *userName, size_t size);

static int g_hMbus = 0;
static int g_hPad = 0;
static int g_hUserService = 0;
static int g_hHidControl = 0;
static int g_vpad_handle = -1;
static bool g_vpad_active = false;
static vpad_rumble_cb_t g_rumble_cb = NULL;

typedef struct {
    int handle;
    bool active;
    VPadSource source;
    uint64_t last_send_time_us;
    OrbisPadData last_sent_data;
    bool has_sent_first;
    int last_ps_state;
    uint32_t last_logged_buttons;
    int logged_insert_packets;
    uint8_t last_rumble_lg;
    uint8_t last_rumble_sm;
    uint64_t last_rumble_send_us;
    int assigned_slot;
    uint64_t device_key;
    uint64_t mbus_device_id;   // what the system binds accounts to; 0 until known
    char assigned_username[64];
} VPadSlotState;

static VPadSlotState g_slots[VPAD_MAX_SLOTS];

static scePadInit_t                          p_scePadInit = NULL;
static scePadMbusInit_t                      p_scePadMbusInit = NULL;
static scePadSetProcessPrivilege_t           p_scePadSetProcessPrivilege = NULL;
static scePadSetConnection_t                 p_scePadSetConnection = NULL;
static scePadVirtualDeviceAddDevice_t        p_scePadVirtualDeviceAddDevice = NULL;
static scePadVirtualDeviceInsertData_t       p_scePadVirtualDeviceInsertData = NULL;
static scePadVirtualDeviceDeleteDevice_t     p_scePadVirtualDeviceDeleteDevice = NULL;
static scePadVirtualDeviceGetRemoteSetting_t p_scePadVirtualDeviceGetRemoteSetting = NULL;
static scePadGetHandle_t                     p_scePadGetHandle = NULL;
static scePadOpen_t                          p_scePadOpen = NULL;
static scePadClose_t                         p_scePadClose = NULL;
static scePadGetDeviceId_t                   p_scePadGetDeviceId = NULL;
static scePadDisconnectPort_t                p_scePadDisconnectPort = NULL;
static scePadDisconnectDevice_t              p_scePadDisconnectDevice = NULL;

static sceMbusInit_t                         p_sceMbusInit = NULL;
static sceMbusBindDeviceWithUserId_t         p_sceMbusBindDeviceWithUserId = NULL;
static sceMbusDisconnectDevice_t             p_sceMbusDisconnectDevice = NULL;
static sceMbusResolveByHandle_t              p_sceMbusResolveByHandle = NULL;

static sceHidControlInit_t                   p_sceHidControlInit = NULL;
static sceHidControlGetDeviceId_t            p_sceHidControlGetDeviceId = NULL;
static sceHidControlDisconnectPort_t         p_sceHidControlDisconnectPort = NULL;
static sceHidControlDisconnectDevice_t       p_sceHidControlDisconnectDevice = NULL;
static sceHidControlDisconnectHandle_t       p_sceHidControlDisconnectHandle = NULL;

static sceUserServiceInitialize_t           p_sceUserServiceInitialize = NULL;
static sceUserServiceGetInitialUser_t       p_sceUserServiceGetInitialUser = NULL;
static sceUserServiceGetLoginUserIdList_t   p_sceUserServiceGetLoginUserIdList = NULL;
static sceUserServiceGetUserName_t          p_sceUserServiceGetUserName = NULL;

static int s_assigned_slot = 1;
static char s_assigned_username[64] = "Player 1";
static int s_last_login_count = -1;

static inline int abs_diff(int a, int b) {
    int d = a - b;
    return d < 0 ? -d : d;
}

static int load_prx(const char *name, int *out_h) {
    char path[256];
    int h = 0;

    // 1. Try full system library path
    snprintf(path, sizeof(path), "/system/common/lib/%s", name);
    if (daemon_sys_load_prx(path, &h) >= 0 && h != 0) {
        *out_h = h;
        return 0;
    }

    // 2. Try raw library name
    if (daemon_sys_load_prx(name, &h) >= 0 && h != 0) {
        *out_h = h;
        return 0;
    }

    return -1;
}

static void init_user_service(void) {
    if (g_hUserService == 0) {
        if (load_prx("libSceUserService.sprx", &g_hUserService) == 0 && g_hUserService != 0) {
            daemon_log(LOG_LEVEL_INFO, "libSceUserService.sprx loaded successfully (handle=0x%X)", g_hUserService);
            daemon_sys_dlsym(g_hUserService, "sceUserServiceInitialize", (void**)&p_sceUserServiceInitialize);
            daemon_sys_dlsym(g_hUserService, "sceUserServiceGetInitialUser", (void**)&p_sceUserServiceGetInitialUser);
            daemon_sys_dlsym(g_hUserService, "sceUserServiceGetLoginUserIdList", (void**)&p_sceUserServiceGetLoginUserIdList);
            daemon_sys_dlsym(g_hUserService, "sceUserServiceGetUserName", (void**)&p_sceUserServiceGetUserName);
            if (p_sceUserServiceInitialize) {
                int r = p_sceUserServiceInitialize(NULL);
                daemon_log(LOG_LEVEL_INFO, "sceUserServiceInitialize() = 0x%08X", r);
            }
        } else {
            daemon_log(LOG_LEVEL_WARN, "Failed to load libSceUserService.sprx");
        }
    }
}

static void init_hid_control(void) {
    if (g_hHidControl == 0) {
        if (load_prx("libSceHidControl.sprx", &g_hHidControl) == 0 && g_hHidControl != 0) {
            daemon_log(LOG_LEVEL_INFO, "libSceHidControl.sprx loaded successfully (handle=0x%X)", g_hHidControl);
            daemon_sys_dlsym(g_hHidControl, "sceHidControlInit", (void**)&p_sceHidControlInit);
            daemon_sys_dlsym(g_hHidControl, "sceHidControlGetDeviceId", (void**)&p_sceHidControlGetDeviceId);
            daemon_sys_dlsym(g_hHidControl, "sceHidControlDisconnectPort", (void**)&p_sceHidControlDisconnectPort);
            daemon_sys_dlsym(g_hHidControl, "sceHidControlDisconnectDevice", (void**)&p_sceHidControlDisconnectDevice);
            daemon_sys_dlsym(g_hHidControl, "sceHidControlDisconnectHandle", (void**)&p_sceHidControlDisconnectHandle);
            if (p_sceHidControlInit) {
                int r = p_sceHidControlInit();
                daemon_log(LOG_LEVEL_INFO, "sceHidControlInit() = 0x%08X", r);
            }
        }
    }
}

static void vpad_poweroff_physical_ds4(void) {
    if (g_daemon_config.mode == CONFIG_MODE_SECONDARY) {
        daemon_log(LOG_LEVEL_INFO, "Skipping physical DS4 power-off (CONFIG_MODE_SECONDARY active for Player 2 co-op)");
        return;
    }

    init_user_service();
    init_hid_control();

    SceUserServiceLoginUserIdList list;
    memset(&list, 0, sizeof(list));
    for (int i = 0; i < SCE_USER_SERVICE_MAX_LOGIN_USERS; i++) {
        list.userId[i] = SCE_USER_SERVICE_USER_ID_INVALID;
    }

    if (p_sceUserServiceGetLoginUserIdList) {
        p_sceUserServiceGetLoginUserIdList(&list);
    }
    if (list.userId[0] == SCE_USER_SERVICE_USER_ID_INVALID && p_sceUserServiceGetInitialUser) {
        p_sceUserServiceGetInitialUser(&list.userId[0]);
    }

    bool any_disconnected = false;
    for (int i = 0; i < SCE_USER_SERVICE_MAX_LOGIN_USERS; i++) {
        SceUserServiceUserId uid = list.userId[i];
        if (uid == SCE_USER_SERVICE_USER_ID_INVALID || uid == 0 || (uint32_t)uid == 0xFFFFFFFF) {
            continue;
        }

        int32_t pad_handle = -1;
        bool opened_temp = false;
        if (p_scePadGetHandle) {
            pad_handle = p_scePadGetHandle(uid, 0, 0);
        }
        if (pad_handle < 0 && p_scePadOpen) {
            pad_handle = p_scePadOpen(uid, 0, 0, NULL);
            if (pad_handle >= 0) {
                opened_temp = true;
            }
        }

        if (pad_handle >= 0 && pad_handle != g_vpad_handle) {
            int64_t dev_id[4] = {0, 0, 0, 0};
            if (p_scePadGetDeviceId) {
                int r_id = p_scePadGetDeviceId(pad_handle, dev_id);
                daemon_log(LOG_LEVEL_INFO, "Physical DS4 [user=0x%X, handle=0x%X]: scePadGetDeviceId() = 0x%08X, dev_id=0x%llX",
                           uid, pad_handle, r_id, (unsigned long long)dev_id[0]);
            }
            if (dev_id[0] == 0 && p_sceHidControlGetDeviceId) {
                p_sceHidControlGetDeviceId(pad_handle, dev_id);
            }
            if (dev_id[0] == 0 && p_sceMbusResolveByHandle) {
                p_sceMbusResolveByHandle(pad_handle, dev_id);
            }

            daemon_log(LOG_LEVEL_INFO, "Powering off physical DS4 for user 0x%X (handle=0x%X, dev_id=0x%X)...",
                       uid, pad_handle, (int32_t)dev_id[0]);

            if (p_scePadDisconnectPort) {
                int r = p_scePadDisconnectPort(pad_handle);
                daemon_log(LOG_LEVEL_INFO, "scePadDisconnectPort(0x%X) = 0x%08X", pad_handle, r);
            }
            if (dev_id[0] > 0) {
                if (p_sceMbusDisconnectDevice) {
                    int r = p_sceMbusDisconnectDevice((int32_t)dev_id[0]);
                    daemon_log(LOG_LEVEL_INFO, "sceMbusDisconnectDevice(0x%X) = 0x%08X", (int32_t)dev_id[0], r);
                }
                if (p_sceHidControlDisconnectDevice) {
                    int r = p_sceHidControlDisconnectDevice((int32_t)dev_id[0]);
                    daemon_log(LOG_LEVEL_INFO, "sceHidControlDisconnectDevice(0x%X) = 0x%08X", (int32_t)dev_id[0], r);
                }
                if (p_scePadDisconnectDevice) {
                    int r = p_scePadDisconnectDevice((int32_t)dev_id[0]);
                    daemon_log(LOG_LEVEL_INFO, "scePadDisconnectDevice(dev_id=0x%X) = 0x%08X", (int32_t)dev_id[0], r);
                }
            }
            if (p_sceHidControlDisconnectPort) {
                int r = p_sceHidControlDisconnectPort(pad_handle);
                daemon_log(LOG_LEVEL_INFO, "sceHidControlDisconnectPort(0x%X) = 0x%08X", pad_handle, r);
            }
            if (p_sceHidControlDisconnectHandle) {
                int r = p_sceHidControlDisconnectHandle(pad_handle);
                daemon_log(LOG_LEVEL_INFO, "sceHidControlDisconnectHandle(0x%X) = 0x%08X", pad_handle, r);
            }
            if (p_scePadDisconnectDevice) {
                int r = p_scePadDisconnectDevice(pad_handle);
                daemon_log(LOG_LEVEL_INFO, "scePadDisconnectDevice(handle=0x%X) = 0x%08X", pad_handle, r);
            }

            if (opened_temp && p_scePadClose) {
                p_scePadClose(pad_handle);
            }
            any_disconnected = true;
        }
    }

    if (any_disconnected) {
        // Wait 100ms for Bluetooth/Mbus stack to complete physical DS4 power-off
        daemon_sys_usleep(100000);
    }
}

int vpad_bind_user(ConfigMode mode, int *out_slot, char *out_username, size_t username_len) {
    init_user_service();

    SceUserServiceLoginUserIdList list;
    memset(&list, 0, sizeof(list));
    for (int i = 0; i < SCE_USER_SERVICE_MAX_LOGIN_USERS; i++) {
        list.userId[i] = SCE_USER_SERVICE_USER_ID_INVALID;
    }

    if (p_sceUserServiceGetLoginUserIdList) {
        int r = p_sceUserServiceGetLoginUserIdList(&list);
        daemon_log(LOG_LEVEL_INFO, "sceUserServiceGetLoginUserIdList() = 0x%08X (u0=0x%X, u1=0x%X, u2=0x%X, u3=0x%X)",
                   r, list.userId[0], list.userId[1], list.userId[2], list.userId[3]);
    }

    int num_logged_in = 0;
    for (int i = 0; i < SCE_USER_SERVICE_MAX_LOGIN_USERS; i++) {
        if (list.userId[i] != SCE_USER_SERVICE_USER_ID_INVALID && list.userId[i] != 0 && (uint32_t)list.userId[i] != 0xFFFFFFFF) {
            num_logged_in++;
        }
    }
    s_last_login_count = num_logged_in;

    SceUserServiceUserId target_user = SCE_USER_SERVICE_USER_ID_INVALID;
    int slot = 1;

    if (mode == CONFIG_MODE_SECONDARY) {
        // Explicitly forced to Player 2
        if (num_logged_in >= 2 && list.userId[1] != SCE_USER_SERVICE_USER_ID_INVALID) {
            target_user = list.userId[1];
            slot = 2;
        } else if (num_logged_in >= 1) {
            target_user = list.userId[0];
            slot = 2; // Tagged as slot 2 for coexistence
        }
    } else if (mode == CONFIG_MODE_MERGE) {
        // Explicitly forced to Player 1
        if (num_logged_in >= 1) {
            target_user = list.userId[0];
            slot = 1;
        }
    } else {
        // CONFIG_MODE_AUTO (default):
        // If 2 or more users logged in (e.g. DS4 is on P1), virtual pad becomes P2!
        if (num_logged_in >= 2 && list.userId[1] != SCE_USER_SERVICE_USER_ID_INVALID) {
            target_user = list.userId[1];
            slot = 2;
        } else if (num_logged_in >= 1) {
            target_user = list.userId[0];
            slot = 1;
        }
    }

    if (target_user == SCE_USER_SERVICE_USER_ID_INVALID && p_sceUserServiceGetInitialUser) {
        p_sceUserServiceGetInitialUser(&target_user);
        slot = 1;
    }

    s_assigned_slot = slot;
    s_assigned_username[0] = '\0';

    if (target_user != SCE_USER_SERVICE_USER_ID_INVALID && p_sceUserServiceGetUserName) {
        p_sceUserServiceGetUserName(target_user, s_assigned_username, sizeof(s_assigned_username));
    }
    if (s_assigned_username[0] == '\0') {
        snprintf(s_assigned_username, sizeof(s_assigned_username), "User %d", slot);
    }

    if (mode != CONFIG_MODE_AUTO && target_user != SCE_USER_SERVICE_USER_ID_INVALID && p_sceMbusBindDeviceWithUserId && g_vpad_handle >= 0) {
        int r = p_sceMbusBindDeviceWithUserId(g_vpad_handle, target_user);
        daemon_log(LOG_LEVEL_INFO, "sceMbusBindDeviceWithUserId(handle=0x%X, user=0x%08X, slot=%d, name='%s') = 0x%08X",
                   g_vpad_handle, target_user, slot, s_assigned_username, r);
    } else {
        daemon_log(LOG_LEVEL_INFO, "Virtual DS4 left for native shell assignment (mode=%d, slot=%d, active User='%s', id=0x%08X)",
                   mode, slot, s_assigned_username, target_user);
    }

    if (out_slot) *out_slot = slot;
    if (out_username && username_len > 0) {
        strncpy(out_username, s_assigned_username, username_len - 1);
        out_username[username_len - 1] = '\0';
    }

    return 0;
}

void vpad_pulse_ps_button_slot(int slot) {
    if (slot < 0 || slot >= VPAD_MAX_SLOTS || !g_slots[slot].active || g_slots[slot].handle < 0 || !p_scePadVirtualDeviceInsertData) {
        return;
    }
    daemon_log(LOG_LEVEL_INFO, "vpad_pulse_ps_button_slot(%d): Sending native PS button pulse (120ms) to trigger user selection...", slot);

    OrbisPadData pulse;
    memset(&pulse, 0, sizeof(pulse));
    pulse.leftStick.x = 128;
    pulse.leftStick.y = 128;
    pulse.rightStick.x = 128;
    pulse.rightStick.y = 128;
    pulse.connected = 1;
    pulse.count = 1;
    pulse.buttons = DS4_BUTTON_PS;
    pulse.timestamp = daemon_sys_time_us();

    // 1. Send PS button pressed
    p_scePadVirtualDeviceInsertData(g_slots[slot].handle, &pulse);
    g_slots[slot].last_send_time_us = pulse.timestamp;
    g_slots[slot].last_sent_data = pulse;
    g_slots[slot].has_sent_first = true;

    // 2. Hold pulse for 120 ms so SceShellCore latches it cleanly
    daemon_sys_usleep(120000);

    // 3. Send PS button released
    pulse.buttons = 0;
    pulse.timestamp = daemon_sys_time_us();
    p_scePadVirtualDeviceInsertData(g_slots[slot].handle, &pulse);
    g_slots[slot].last_send_time_us = pulse.timestamp;
    g_slots[slot].last_sent_data = pulse;

    daemon_log(LOG_LEVEL_INFO, "vpad_pulse_ps_button_slot(%d): PS button released, native user picker launched", slot);
}

void vpad_pulse_ps_button(void) {
    for (int i = 0; i < VPAD_MAX_SLOTS; i++) {
        if (g_slots[i].active) {
            vpad_pulse_ps_button_slot(i);
            return;
        }
    }
}

int vpad_get_assigned_slot(void) {
    return s_assigned_slot;
}

const char* vpad_get_assigned_user(void) {
    return s_assigned_username;
}

void vpad_check_user_rebind(ConfigMode mode) {
    if (!g_vpad_active || g_vpad_handle < 0 || mode != CONFIG_MODE_AUTO || !p_sceUserServiceGetLoginUserIdList) {
        return;
    }

    SceUserServiceLoginUserIdList list;
    memset(&list, 0, sizeof(list));
    if (p_sceUserServiceGetLoginUserIdList(&list) == 0) {
        int num_logged_in = 0;
        for (int i = 0; i < SCE_USER_SERVICE_MAX_LOGIN_USERS; i++) {
            if (list.userId[i] != SCE_USER_SERVICE_USER_ID_INVALID && list.userId[i] != 0 && (uint32_t)list.userId[i] != 0xFFFFFFFF) {
                num_logged_in++;
            }
        }
        if (num_logged_in != s_last_login_count) {
            daemon_log(LOG_LEVEL_INFO, "Logged in user count changed (%d -> %d), updating virtual pad binding...",
                       s_last_login_count, num_logged_in);
            vpad_bind_user(mode, NULL, NULL, 0);
        }
    }
}

static int vpad_ensure_subsystem(void) {
    // 1. Mandatory: libSceMbus must be loaded prior to libScePad
    if (g_hMbus == 0) {
        if (load_prx("libSceMbus.sprx", &g_hMbus) < 0) {
            daemon_log(LOG_LEVEL_WARN, "Failed to load libSceMbus.sprx");
        } else {
            daemon_log(LOG_LEVEL_INFO, "libSceMbus.sprx loaded successfully (handle=0x%X)", g_hMbus);
            daemon_sys_dlsym(g_hMbus, "sceMbusInit", (void**)&p_sceMbusInit);
            daemon_sys_dlsym(g_hMbus, "sceMbusBindDeviceWithUserId", (void**)&p_sceMbusBindDeviceWithUserId);
            daemon_sys_dlsym(g_hMbus, "sceMbusDisconnectDevice", (void**)&p_sceMbusDisconnectDevice);
            daemon_sys_dlsym(g_hMbus, "sceMbusResolveByHandle", (void**)&p_sceMbusResolveByHandle);
            if (p_sceMbusInit) {
                int r = p_sceMbusInit();
                daemon_log(LOG_LEVEL_INFO, "sceMbusInit() = 0x%08X", r);
            }
        }
    }

    // 2. Load libScePad and initialize Pad/Mbus once
    if (g_hPad == 0) {
        if (load_prx("libScePad.sprx", &g_hPad) < 0) {
            daemon_log(LOG_LEVEL_ERROR, "Failed to load libScePad.sprx");
            return -1;
        }
        daemon_log(LOG_LEVEL_INFO, "libScePad.sprx loaded successfully (handle=0x%X)", g_hPad);

        // 3. Resolve Pad Symbols
        daemon_sys_dlsym(g_hPad, "scePadInit", (void**)&p_scePadInit);
        daemon_sys_dlsym(g_hPad, "scePadMbusInit", (void**)&p_scePadMbusInit);
        daemon_sys_dlsym(g_hPad, "scePadSetProcessPrivilege", (void**)&p_scePadSetProcessPrivilege);
        daemon_sys_dlsym(g_hPad, "scePadSetConnection", (void**)&p_scePadSetConnection);
        daemon_sys_dlsym(g_hPad, "scePadVirtualDeviceAddDevice", (void**)&p_scePadVirtualDeviceAddDevice);
        daemon_sys_dlsym(g_hPad, "scePadVirtualDeviceInsertData", (void**)&p_scePadVirtualDeviceInsertData);
        daemon_sys_dlsym(g_hPad, "scePadVirtualDeviceDeleteDevice", (void**)&p_scePadVirtualDeviceDeleteDevice);
        daemon_sys_dlsym(g_hPad, "scePadVirtualDeviceGetRemoteSetting", (void**)&p_scePadVirtualDeviceGetRemoteSetting);
        daemon_sys_dlsym(g_hPad, "scePadGetHandle", (void**)&p_scePadGetHandle);
        daemon_sys_dlsym(g_hPad, "scePadOpen", (void**)&p_scePadOpen);
        daemon_sys_dlsym(g_hPad, "scePadClose", (void**)&p_scePadClose);
        daemon_sys_dlsym(g_hPad, "scePadGetDeviceId", (void**)&p_scePadGetDeviceId);
        daemon_sys_dlsym(g_hPad, "scePadDisconnectPort", (void**)&p_scePadDisconnectPort);
        daemon_sys_dlsym(g_hPad, "scePadDisconnectDevice", (void**)&p_scePadDisconnectDevice);

        // Fallback: check if sceMbusBindDeviceWithUserId is exported from libScePad
        if (!p_sceMbusBindDeviceWithUserId) {
            daemon_sys_dlsym(g_hPad, "sceMbusBindDeviceWithUserId", (void**)&p_sceMbusBindDeviceWithUserId);
        }

        if (p_scePadInit) {
            int r = p_scePadInit();
            daemon_log(LOG_LEVEL_INFO, "scePadInit() = 0x%08X", r);
        }
        if (p_scePadMbusInit) {
            int r = p_scePadMbusInit();
            daemon_log(LOG_LEVEL_INFO, "scePadMbusInit() = 0x%08X", r);
        }
        if (p_scePadSetProcessPrivilege) {
            int r = p_scePadSetProcessPrivilege(1);
            daemon_log(LOG_LEVEL_INFO, "scePadSetProcessPrivilege(1) = 0x%08X", r);
        }
    }

    if (!p_scePadVirtualDeviceAddDevice || !p_scePadVirtualDeviceInsertData) {
        daemon_log(LOG_LEVEL_ERROR, "VirtualDevice APIs not exported by libScePad");
        return -2;
    }

    return 0;
}

/* ---- which account owns each pad, read out of the kernel log ----
 *
 * libScePad hands back a handle (0x6030D, 0x8030C) that is NOT the MBus DeviceId
 * the system binds accounts to (0x90300). Different namespaces, and nothing in
 * userland maps one to the other, so this daemon has had no way to say which
 * account owns which pad -- the question that matters when a controller set up
 * as Guest ends up driving the main user's screen.
 *
 * The login manager announces both facts in the kernel log and nowhere else:
 *
 *   SCE_MBUS_EVENT_DEVICE_ADDED [DeviceId:0x90300][type:1][subType:2]
 *   [Assign] DeviceId:0x0000000000090300 / UserId:0x20b7c46a
 *   SCE_MBUS_EVENT_DEVICE_OWNER_CHANGED [DeviceId:0x90300][UserId:0x20b7c46a]
 *
 * subType 2 is the virtual (Remote Play) pad, which is what we create, so the
 * first line ties a DeviceId to the slot we just opened and the other two tie
 * that DeviceId to an account. Both then go into this daemon's own log next to
 * the slot number, which is the point: one log, one clock, whole story. Reading
 * the kernel log for this is the technique described in Control4Free's docs; it
 * is GPL-3.0 and none of its code is used here.
 *
 * /dev/klog has a single reader: while this build holds it, GoldHEN's log server
 * on port 3232 cannot serve anyone. That is why it is behind a build flag.
 */
#ifdef ONEFAMILY_OWNER_WATCH

static int      s_klog_fd = -1;
static int      s_klog_pending_slot = -1;   /* slot still waiting for its DeviceId */
static char     s_klog_line[1024];
static size_t   s_klog_line_len = 0;
static uint64_t s_klog_retry_us = 0;

/* The hex number following `marker`, with or without an 0x prefix. */
static uint64_t vpad_hex_after(const char *s, const char *marker) {
    const char *p = strstr(s, marker);
    if (!p) return 0;
    p += strlen(marker);
    while (*p == ' ') ++p;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;

    uint64_t v = 0;
    int n = 0;
    for (; n < 16; ++n, ++p) {
        int d;
        char c = *p;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | (uint64_t)d;
    }
    return n ? v : 0;
}

static int vpad_slot_of_device(uint64_t device_id) {
    for (int i = 0; i < VPAD_MAX_SLOTS; i++) {
        if (g_slots[i].active && g_slots[i].mbus_device_id == device_id) return i;
    }
    return -1;
}

static void vpad_name_of_user(uint32_t user_id, char *out, size_t size) {
    out[0] = '\0';
    init_user_service();
    if (p_sceUserServiceGetUserName) {
        p_sceUserServiceGetUserName((SceUserServiceUserId)user_id, out, size);
    }
    if (out[0] == '\0') {
        snprintf(out, size, "0x%08X", user_id);
    }
}

static void vpad_klog_handle_line(const char *line) {
    /* A virtual pad was created: claim it for the slot that is waiting. */
    if (strstr(line, "SCE_MBUS_EVENT_DEVICE_ADDED") && strstr(line, "subType:2")) {
        uint64_t id = vpad_hex_after(line, "DeviceId:");
        if (!id) return;

        int slot = s_klog_pending_slot;
        if (slot >= 0 && slot < VPAD_MAX_SLOTS && g_slots[slot].active) {
            g_slots[slot].mbus_device_id = id;
            s_klog_pending_slot = -1;
            daemon_log(LOG_LEVEL_INFO,
                       "owner_watch: Slot %d (handle=0x%X) is MBus DeviceId 0x%llX",
                       slot, g_slots[slot].handle, (unsigned long long)id);
        } else {
            daemon_log(LOG_LEVEL_INFO,
                       "owner_watch: virtual pad 0x%llX added with no slot waiting for it",
                       (unsigned long long)id);
        }
        return;
    }

    int is_assign = (strstr(line, "[Assign] DeviceId:") != NULL);
    int is_owner  = (strstr(line, "SCE_MBUS_EVENT_DEVICE_OWNER_CHANGED") != NULL);
    if (!is_assign && !is_owner) return;

    uint64_t id  = vpad_hex_after(line, "DeviceId:");
    uint64_t uid = vpad_hex_after(line, "UserId:");
    if (!id) return;

    char name[64];
    vpad_name_of_user((uint32_t)uid, name, sizeof(name));
    int slot = vpad_slot_of_device(id);

    if (slot >= 0) {
        daemon_log(LOG_LEVEL_INFO,
                   "owner_watch: Slot %d (Player %d, MBus 0x%llX) belongs to '%s' (0x%08X) [%s]",
                   slot, g_slots[slot].assigned_slot, (unsigned long long)id,
                   name, (uint32_t)uid, is_owner ? "owner_changed" : "assign");
    } else {
        daemon_log(LOG_LEVEL_INFO,
                   "owner_watch: device 0x%llX belongs to '%s' (0x%08X) [%s, not one of our pads]",
                   (unsigned long long)id, name, (uint32_t)uid,
                   is_owner ? "owner_changed" : "assign");
    }
}

/* Throw away whatever is already queued, so the first poll does not report a
 * backlog of events from before this daemon started. */
static void vpad_klog_drain(void) {
    char chunk[1024];
    int guard = 0;
    while (guard++ < 256) {
        int n = daemon_sys_read(s_klog_fd, chunk, sizeof(chunk));
        if (n <= 0) break;
    }
    s_klog_line_len = 0;
}

void vpad_owner_watch_poll(void) {
    if (s_klog_fd < 0) {
        uint64_t now = daemon_sys_time_us();
        if (now < s_klog_retry_us) return;
        s_klog_retry_us = now + 5000000ULL;

        /* O_RDONLY | O_NONBLOCK */
        s_klog_fd = daemon_sys_open("/dev/klog", 0x0004, 0);
        if (s_klog_fd < 0) {
            daemon_log(LOG_LEVEL_WARN,
                       "owner_watch: /dev/klog unavailable (GoldHEN's log server may hold it); will retry");
            return;
        }
        vpad_klog_drain();
        daemon_log(LOG_LEVEL_INFO, "owner_watch: reading /dev/klog, backlog discarded");
    }

    char chunk[1024];
    int rounds = 0;
    while (rounds++ < 64) {
        int n = daemon_sys_read(s_klog_fd, chunk, sizeof(chunk));
        if (n <= 0) break;

        for (int i = 0; i < n; ++i) {
            char c = chunk[i];
            if (c == '\n' || c == '\r') {
                if (s_klog_line_len) {
                    s_klog_line[s_klog_line_len] = '\0';
                    vpad_klog_handle_line(s_klog_line);
                    s_klog_line_len = 0;
                }
            } else if (s_klog_line_len + 1 < sizeof(s_klog_line)) {
                s_klog_line[s_klog_line_len++] = c;
            }
        }
        if (n < (int)sizeof(chunk)) break;
    }
}

void vpad_owner_note_pad_open(int slot) {
    s_klog_pending_slot = slot;
}

#else  /* ONEFAMILY_OWNER_WATCH */

void vpad_owner_watch_poll(void) {}
void vpad_owner_note_pad_open(int slot) { (void)slot; }

#endif /* ONEFAMILY_OWNER_WATCH */

int vpad_slot_open_dev(VPadSource source, uint64_t device_key, int *out_slot) {
    if (vpad_ensure_subsystem() != 0) {
        return -1;
    }

    // Check whether this physical device already holds a slot. Matching on the
    // device key as well as the source is what gives a second DualSense a slot
    // of its own instead of making it share the first one's virtual pad.
    for (int i = 0; i < VPAD_MAX_SLOTS; i++) {
        if (g_slots[i].active && g_slots[i].source == source && g_slots[i].device_key == device_key) {
            if (out_slot) *out_slot = i;
            return 0;
        }
    }

    // Find first free slot
    int slot = -1;
    for (int i = 0; i < VPAD_MAX_SLOTS; i++) {
        if (!g_slots[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        daemon_log(LOG_LEVEL_WARN, "vpad_slot_open: All %d virtual pad slots are occupied", VPAD_MAX_SLOTS);
        return -1;
    }

    // Power off physical DS4 before opening the first virtual pad so its link drops cleanly
    bool any_prior_active = false;
    for (int i = 0; i < VPAD_MAX_SLOTS; i++) {
        if (g_slots[i].active) { any_prior_active = true; break; }
    }
    if (!any_prior_active) {
        vpad_poweroff_physical_ds4();
    }

    ScePadVirtualDeviceParam param;
    memset(&param, 0, sizeof(param));
    param.size = sizeof(param);
    param.userId = 1;   // unassigned: the PS4 opens its user picker for this pad
    param.flags = 0;

    int vHandle = -1;
    int ret = p_scePadVirtualDeviceAddDevice(&param, &vHandle);
    daemon_log(LOG_LEVEL_INFO,
               "AddDevice(size=%u, userId=%u) = 0x%08X, handle=0x%X",
               param.size, param.userId, (uint32_t)ret, vHandle);
    if (ret < 0 || vHandle < 0) {
        daemon_log(LOG_LEVEL_ERROR, "scePadVirtualDeviceAddDevice(type 1) failed for slot %d: 0x%08X", slot, ret);
        return ret;
    }

    memset(&g_slots[slot], 0, sizeof(VPadSlotState));
    g_slots[slot].handle = vHandle;
    g_slots[slot].active = true;
    g_slots[slot].source = source;
    g_slots[slot].assigned_slot = slot + 1;
    g_slots[slot].device_key = device_key;
    g_slots[slot].mbus_device_id = 0;
    vpad_owner_note_pad_open(slot);
    snprintf(g_slots[slot].assigned_username, sizeof(g_slots[slot].assigned_username), "User %d", slot + 1);

    g_vpad_active = true;
    g_vpad_handle = vHandle;

    daemon_log(LOG_LEVEL_INFO, "Virtual DS4 Slot %d (%s, key=0x%llX) registered with handle=0x%X -> Player %d",
               slot, source == VPAD_SOURCE_BT ? "Bluetooth" : "USB",
               (unsigned long long)device_key, vHandle, g_slots[slot].assigned_slot);

    if (p_scePadSetConnection) {
        int r = p_scePadSetConnection(vHandle, 1);
        daemon_log(LOG_LEVEL_INFO, "scePadSetConnection(0x%X, 1) = 0x%08X", vHandle, r);
    }

    // Pulse PS so SceShellCore opens its native "Who is using this
    // controller?" picker for this pad.
    vpad_pulse_ps_button_slot(slot);

    if (out_slot) *out_slot = slot;
    return 0;
}

int vpad_slot_open(VPadSource source, int *out_slot) {
    return vpad_slot_open_dev(source, 0, out_slot);
}

void vpad_slot_close(int slot) {
    if (slot < 0 || slot >= VPAD_MAX_SLOTS || !g_slots[slot].active) {
        return;
    }

    int handle = g_slots[slot].handle;
    daemon_log(LOG_LEVEL_INFO, "Closing Virtual Pad Slot %d (handle=0x%X, source=%d)",
               slot, handle, g_slots[slot].source);

    if (handle >= 0) {
        if (p_scePadSetConnection) {
            p_scePadSetConnection(handle, 0);
        }
        if (p_sceMbusDisconnectDevice) {
            p_sceMbusDisconnectDevice(handle);
        }
        daemon_sys_usleep(30000);
        if (p_scePadVirtualDeviceDeleteDevice) {
            p_scePadVirtualDeviceDeleteDevice(handle);
        }
    }

    memset(&g_slots[slot], 0, sizeof(VPadSlotState));
    g_slots[slot].handle = -1;

    bool any_active = false;
    for (int i = 0; i < VPAD_MAX_SLOTS; i++) {
        if (g_slots[i].active) {
            any_active = true;
            g_vpad_handle = g_slots[i].handle;
            break;
        }
    }
    if (!any_active) {
        g_vpad_active = false;
        g_vpad_handle = -1;
    }
}

bool vpad_slot_is_active(int slot) {
    if (slot >= 0 && slot < VPAD_MAX_SLOTS) {
        return g_slots[slot].active;
    }
    return false;
}

int vpad_get_assigned_slot_num(int slot) {
    if (slot >= 0 && slot < VPAD_MAX_SLOTS && g_slots[slot].active) {
        return g_slots[slot].assigned_slot;
    }
    return 1;
}

int vpad_init(void) {
    int slot = -1;
    return vpad_slot_open(VPAD_SOURCE_USB, &slot);
}

void vpad_shutdown(void) {
    for (int i = 0; i < VPAD_MAX_SLOTS; i++) {
        if (g_slots[i].active) {
            vpad_slot_close(i);
        }
    }
    g_vpad_active = false;
    g_vpad_handle = -1;
}

bool vpad_is_active(void) {
    return g_vpad_active;
}

int vpad_send_data_slot(int slot, const OrbisPadData *data) {
    if (slot < 0 || slot >= VPAD_MAX_SLOTS || !g_slots[slot].active || g_slots[slot].handle < 0 || !p_scePadVirtualDeviceInsertData || !data) {
        return -1;
    }

    VPadSlotState *s = &g_slots[slot];
    uint64_t now = daemon_sys_time_us();

    // Determine if pad is in neutral / resting state
    bool is_neutral = (data->buttons == 0 &&
                       data->analogButtons.l2 <= 4 &&
                       data->analogButtons.r2 <= 4 &&
                       abs_diff(data->leftStick.x, 128) <= 4 &&
                       abs_diff(data->leftStick.y, 128) <= 4 &&
                       abs_diff(data->rightStick.x, 128) <= 4 &&
                       abs_diff(data->rightStick.y, 128) <= 4);

    bool state_changed = false;
    if (!s->has_sent_first) {
        state_changed = true;
    } else if (data->buttons != s->last_sent_data.buttons) {
        state_changed = true;
    } else if (abs_diff(data->leftStick.x, s->last_sent_data.leftStick.x) > 1 ||
               abs_diff(data->leftStick.y, s->last_sent_data.leftStick.y) > 1 ||
               abs_diff(data->rightStick.x, s->last_sent_data.rightStick.x) > 1 ||
               abs_diff(data->rightStick.y, s->last_sent_data.rightStick.y) > 1) {
        state_changed = true;
    } else if (abs_diff(data->analogButtons.l2, s->last_sent_data.analogButtons.l2) > 2 ||
               abs_diff(data->analogButtons.r2, s->last_sent_data.analogButtons.r2) > 2) {
        state_changed = true;
    }

    if (!state_changed) {
        uint64_t interval = is_neutral ? 200000 : 20000;
        if ((now - s->last_send_time_us) < interval) {
            return 0; // Throttled cleanly
        }
    }

    int ret = p_scePadVirtualDeviceInsertData(s->handle, data);
    s->last_send_time_us = now;
    s->last_sent_data = *data;
    s->has_sent_first = true;

    int ps_now = (data->buttons & DS4_BUTTON_PS) ? 1 : 0;
    if (ps_now != s->last_ps_state) {
        s->last_ps_state = ps_now;
        daemon_log(LOG_LEVEL_INFO, "vpad_send_data_slot(%d): PS Button %s (btn=0x%08X, ret=0x%08X)",
                   slot, ps_now ? "ACTIVE" : "RELEASED", data->buttons, ret);
    }

    if (s->logged_insert_packets < 5 || (ret != 0 && s->logged_insert_packets < 25) ||
        (data->buttons != s->last_logged_buttons && s->logged_insert_packets < 60)) {
        daemon_log(LOG_LEVEL_INFO, "InsertData(slot=%d, h=0x%X, btn=0x%08X, lx=%d, ly=%d) -> 0x%08X",
                   slot, s->handle, data->buttons, data->leftStick.x, data->leftStick.y, ret);
        s->last_logged_buttons = data->buttons;
        s->logged_insert_packets++;
    }

    return ret;
}

int vpad_send_data(const OrbisPadData *data) {
    for (int i = 0; i < VPAD_MAX_SLOTS; i++) {
        if (g_slots[i].active) {
            return vpad_send_data_slot(i, data);
        }
    }
    return -1;
}

void vpad_set_rumble_callback(vpad_rumble_cb_t cb) {
    g_rumble_cb = cb;
}

void vpad_poll_rumble(void) {
    if (!p_scePadVirtualDeviceGetRemoteSetting) return;

    for (int i = 0; i < VPAD_MAX_SLOTS; i++) {
        if (!g_slots[i].active || g_slots[i].handle < 0) continue;

        uint8_t setting[128];
        memset(setting, 0, sizeof(setting));
        int ret = p_scePadVirtualDeviceGetRemoteSetting(g_slots[i].handle, setting);
        if (ret == 0) {
            uint8_t lg = setting[3];
            uint8_t sm = setting[4];
            uint64_t now = daemon_sys_time_us();

            bool changed = (lg != g_slots[i].last_rumble_lg || sm != g_slots[i].last_rumble_sm);
            bool keepalive = ((lg > 0 || sm > 0) && (now - g_slots[i].last_rumble_send_us >= 100000ULL));

            if (changed || keepalive) {
                g_slots[i].last_rumble_lg = lg;
                g_slots[i].last_rumble_sm = sm;
                g_slots[i].last_rumble_send_us = now;

                if (g_slots[i].source == VPAD_SOURCE_BT) {
                    /* enable_rumble was only ever honored on the USB path, so a
                     * Bluetooth DualSense rumbled whatever the setting said.
                     *
                     * It matters beyond preference: this is the only Bluetooth
                     * *output* traffic the daemon produces -- a 78-byte report
                     * on every change and every 100 ms while a motor runs, per
                     * controller. With two DualSense contending for the radio,
                     * and input streams measured going silent for seconds at a
                     * time, switching it off is a real lever on link stability.
                     * The light bar colour rides in the same report and stops
                     * with it. */
                    if (g_daemon_config.enable_rumble) {
                        uint8_t color[3] = { setting[5], setting[6], setting[7] };
                        bt_driver_send_rumble_slot(i, lg, sm, color);
                    }
                } else if (g_slots[i].source == VPAD_SOURCE_USB) {
                    if (g_rumble_cb) {
                        g_rumble_cb(lg, sm);
                    } else {
                        usb_driver_send_rumble(lg, sm);
                    }
                }
            }
        }
    }
}
