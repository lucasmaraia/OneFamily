#ifndef ONEFAMILY_BT_DRIVER_H
#define ONEFAMILY_BT_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

/* How many DualSense controllers may be connected over Bluetooth at once.
 * Capped by the PS4's own four-player limit (see VPAD_MAX_SLOTS). */
#define BT_MAX_DEVICES 4

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the DualSense Bluetooth Driver Subsystem.
 * Performs kernel allowlist patching (via SigScan / Firmware matrix)
 * and starts the background /dev/bt listener thread.
 *
 * @return 0 on success, negative error code on failure.
 */
int bt_driver_init(void);

/**
 * Non-blocking poll step executed in the main daemon loop (250 Hz).
 * Checks kqueue for /dev/bt events, queries reports via ioctl 0x80206233,
 * decodes input and feeds each controller's own virtual DualShock 4.
 *
 * Every /dev/bt event carries the Bluetooth address of the controller it came
 * from, so reports are routed per device rather than merged into one pad.
 */
void bt_driver_poll_step(void);

/**
 * Shutdown the Bluetooth Driver Subsystem and restore original kernel allowlist.
 */
void bt_driver_shutdown(void);

/**
 * Check if the DualSense Bluetooth Subsystem is active and supported on this firmware.
 */
bool bt_driver_is_supported(void);

/**
 * Check if at least one DualSense controller is connected via Bluetooth.
 */
bool bt_driver_is_connected(void);

/**
 * Number of DualSense controllers currently connected over Bluetooth.
 */
int bt_driver_connected_count(void);

/**
 * Send rumble and LED color feedback to the first connected DualSense.
 * Kept for callers that have no slot to hand over, such as the companion app's
 * vibration test; with several controllers connected, prefer the slot variant.
 *
 * @param large Large rumble motor intensity (0-255).
 * @param small Small rumble motor intensity (0-255).
 * @param color RGB lightbar color [R, G, B].
 */
void bt_driver_send_rumble(uint8_t large, uint8_t small, const uint8_t color[3]);

/**
 * Send rumble and LED feedback to the DualSense bound to a given virtual pad
 * slot. This is what the per-slot rumble poll uses: with several DualSense
 * connected, feedback has to reach the controller that owns that slot, not
 * whichever one happens to be first.
 *
 * @param vpad_slot Virtual pad slot index, as returned by vpad_slot_open_dev().
 * @param large Large rumble motor intensity (0-255).
 * @param small Small rumble motor intensity (0-255).
 * @param color RGB lightbar color [R, G, B].
 */
void bt_driver_send_rumble_slot(int vpad_slot, uint8_t large, uint8_t small, const uint8_t color[3]);

#ifdef __cplusplus
}
#endif

#endif // ONEFAMILY_BT_DRIVER_H
