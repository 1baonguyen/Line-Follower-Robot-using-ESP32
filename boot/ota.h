#pragma once

/* Call first thing at start-up. Opens Wi-Fi + upload page for OTA_WINDOW_MS.
 * An uploaded .bin is flashed and the robot reboots into it.
 * BOOT button or timeout = continue with the normal program. */
void ota_boot_window(void);