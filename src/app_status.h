/*
 * app_status – a small snapshot of what every task is doing, for the
 * console's "status" command. Each task writes its own fields; nothing here
 * drives behaviour.
 *
 * Writers:  app_status_t *s = app_status_lock(); s->claps++; app_status_unlock();
 * Keep the locked section to plain assignments and short string copies: the
 * lock is a critical section (interrupts are masked on that core).
 */
#ifndef APP_STATUS_H
#define APP_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_STATUS_TEXT_MAX 64

typedef struct {
    /* control_task */
    bool ambient;
    bool transitioning;
    uint8_t volume_step;
    uint8_t volume_max;
    bool amp_powered;
    uint8_t amp_i2c_addr;      /* 0 = not found yet, or log-only callbacks */
    uint32_t amp_faults;       /* fault reports since boot */
    /* mic_task */
    bool mic_listening;
    uint32_t claps;            /* confirmed double claps since boot */
    uint32_t last_clap_ms;
    /* wifi */
    bool wifi_connected;
    char wifi_ip[16];
    /* hue_task */
    char hue_state[24];
    char hue_last[24];
    /* player_task */
    bool sd_mounted;
    uint16_t tracks;
    bool playing;
    char track[APP_STATUS_TEXT_MAX];
} app_status_t;

app_status_t *app_status_lock(void);
void app_status_unlock(void);

/* A consistent copy of the whole snapshot. */
void app_status_get(app_status_t *out);

/* Bounded copy into one of the text fields (call while locked). */
void app_status_copy(char *dst, size_t dst_size, const char *src);

#endif /* APP_STATUS_H */
