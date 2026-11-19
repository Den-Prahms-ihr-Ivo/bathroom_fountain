/*
 * console_task – serial console for the settings (CLAP-12, NFR-10, NFR-15).
 *
 *   set <key> <value>   everything after the single space following the key
 *                       is the value, verbatim (spaces in an SSID are kept);
 *                       "set wifi_pass" with nothing after it clears it
 *   get <key>           secrets are shown as ********
 *   show                all keys
 *   status              mode, volume, amplifier, mic, Wi-Fi, Hue, music
 *   hue pair            pair with the bridge (press its link button first)
 *   hue scenes          list the bridge's scenes with their ids
 *   hue normal|ambient  recall that scene now (bring-up test)
 *   next | rescan       next track | scan the SD card again
 *   reboot
 *   help
 *
 * Typing works on the UART console port and, when the USB-Serial-JTAG port
 * mirrors the console (the ESP32-S3 default), on the native USB port too.
 * Every accepted set is saved to NVS at once; a Wi-Fi key reconnects.
 */
#ifndef CONSOLE_TASK_H
#define CONSOLE_TASK_H

/* Starts the task (core 0). Settings must be loaded first. */
void console_task_start(void);

#endif /* CONSOLE_TASK_H */
