/*
 * bed_modes – the bedside's music modes: meditation, mood and sleep, plus the
 * "next" and "stop" buttons (pure C, no hardware).
 *
 * Requirements: BED-40 ... BED-53 (see the requirements doc).
 * Tests:        test/test_bed_modes/
 *
 * The module decides; the bedside app carries it out. It never sees folder
 * names, scene ids or volumes: its commands name the mode ("play the sleep
 * folder", "recall the meditation scene") and the app looks the rest up in
 * the settings (sleep_folder, scene_sleep, ...) and in lib/mode_volume, then
 * sends LINK_AUDIO to the speaker and the scene to hue_task. Candles are a
 * GPIO on the bedside (outputs.candles).
 *
 * Time: now_ms is a wrapping uint32 millisecond counter. Every call that
 * takes now_ms first brings the state up to date: timers that ran out since
 * the last call take effect at their planned times, in time order, so a late
 * call ends where regular ticking would have (the commands they caused are
 * queued in that order).
 *
 * Modes (outputs.mode)
 *   BED_MODE_NONE        nothing plays. The encoder is ignored (lib/mode_volume).
 *   BED_MODE_MEDITATION  meditation folder, meditation scene, candles on.
 *   BED_MODE_MOOD        mood folder, mood scene, candles on. No timer.
 *   BED_MODE_SLEEP       sleep folder; scene and candles only at the first
 *                        press of a night (below).
 *   A mode stays the mode during its closing fade (outputs.fading is then
 *   true), so the encoder still sets that mode's volume while it fades.
 *   The newest button wins: a mode button while another mode runs ends that
 *   mode at once (no fade, its timers are dropped, candles it turned on go
 *   off unless the new mode turns them on) and starts the new one; its PLAY
 *   replaces the music on the speaker.
 *
 * Commands (bed_modes_take_cmd(), oldest first; at most BED_CMDS wait, the
 * oldest is dropped when full). Which are issued is specified per button:
 *   BED_CMD_SCENE  recall scene `scene` over `fade_ms`.
 *   BED_CMD_PLAY   play the folder of mode `folder`, fading in over
 *                  `fade_ms`. resume: continue where the last KEEP session
 *                  stopped (exact track and position). keep: the speaker
 *                  remembers where this session stops. Sleep is played with
 *                  keep = true, the others with keep = false.
 *   BED_CMD_STOP   fade the music out over `fade_ms`, then stop.
 *   BED_CMD_NEXT   next track.
 *   BED_CMD_MOOD_FOLDER_NEXT  the app moves its mood folder to the next
 *                  "mood_" folder (bed_next_folder()) and stores it; the
 *                  BED_CMD_PLAY that follows plays the new folder.
 *   BED_CMD_SLEEP_FOLDER_NEXT the same for the sleep folder and "sleep_".
 *   Fields a command does not use are 0 / false (scene: BED_SCENE_NORMAL,
 *   folder: BED_MODE_NONE).
 *
 * Buttons (bed_modes_button(); returns true if the press did anything)
 *   While the alarm rings (bed_modes_alarm(true)) every button is ignored:
 *   returns false, nothing changes, no command.
 *
 *   MEDITATION
 *     mode is MEDITATION and not fading: the timer restarts (cfg.med_ms from
 *       now). No command.
 *     otherwise (any other mode, NONE, or MEDITATION while fading):
 *       SCENE(MEDITATION, start_fade_ms), PLAY(MEDITATION, resume false,
 *       keep false, start_fade_ms); candles on; mode MEDITATION, not fading;
 *       timer cfg.med_ms from now.
 *   MOOD
 *     mode is MOOD: MOOD_FOLDER_NEXT, then PLAY(MOOD, false, false,
 *       start_fade_ms). Scene and candles are left as they are.
 *     otherwise: SCENE(MOOD, start_fade_ms), PLAY(MOOD, false, false,
 *       start_fade_ms); candles on; mode MOOD. Mood has no timer.
 *   SLEEP
 *     mode is SLEEP and not fading: like mood, the next folder:
 *       SLEEP_FOLDER_NEXT, then PLAY(SLEEP, resume true, keep true,
 *       start_fade_ms) (each sleep folder continues where it stopped, if it
 *       has played before). The sleep timer keeps running from where it
 *       was (switching is not extending); candles and their timer are left
 *       alone. The first press of a night plays the current folder from a
 *       random track (see below).
 *     mode is SLEEP and fading: the fade is cancelled: PLAY(SLEEP, resume
 *       true, keep true, start_fade_ms); timer cfg.sleep_ms from now.
 *       Candles left alone.
 *     otherwise, a continuation (see Nights): PLAY(SLEEP, resume true, keep
 *       true, start_fade_ms). No scene. Candles off (whatever turned them
 *       on). Mode SLEEP, timer cfg.sleep_ms from now.
 *     otherwise, the first press of a night: SCENE(SLEEP, start_fade_ms),
 *       PLAY(SLEEP, resume false, keep true, start_fade_ms). Candles on for
 *       cfg.sleep_candle_ms (0: candles off). Mode SLEEP, timer
 *       cfg.sleep_ms from now. This press is now the night's first.
 *   NEXT
 *     mode is not NONE: NEXT (also while fading). Otherwise ignored.
 *   STOP
 *     mode is not NONE: STOP(cfg.stop_fade_ms), mode NONE at once, timers
 *       dropped, candles off. Hue is left alone.
 *     mode NONE but candles on: candles off (candle timer dropped).
 *     neither: ignored. STOP does not touch the night: the next SLEEP press
 *     of the same night is still a continuation.
 *
 * Timers
 *   Meditation timer runs out: STOP(cfg.med_fade_ms) and SCENE(NORMAL,
 *     cfg.med_fade_ms); fading = true. When the fade is over: candles off,
 *     mode NONE. With med_fade_ms 0 all of this happens at the timer's end.
 *   Sleep timer runs out (cfg.sleep_ms 0 = no timer): STOP(cfg.sleep_fade_ms);
 *     fading = true; when the fade is over: mode NONE. The scene stays.
 *   Sleep candle timer runs out: candles off. It runs on its own: it is not
 *     stopped by the sleep timer, only by STOP, by another mode, by the alarm
 *     and by a continuation.
 *   The meditation timer restart above restarts the mode timer only, never
 *   a fade that has begun. SLEEP during its fade cancels the fade and starts
 *   a new sleep timer, as listed.
 *
 * Nights
 *   The app passes `night` with every press: bed_night_id() of the local
 *   time, or BED_NIGHT_UNKNOWN while the clock is not set. A night runs from
 *   12:00 to 12:00 local time. A SLEEP press that is not a timer restart or
 *   a fade cancel is a continuation if
 *     night is known and equals the night of the last first press, or
 *     night is unknown and the last first press was less than 12 h ago
 *     (by now_ms, in this object's life).
 *   bed_modes_sleep_night() is the night of the last first press (or the
 *   value given to create()); the app saves it in NVS whenever it changes,
 *   so a reboot during the night keeps the continuation. A first press made
 *   with an unknown night leaves the stored night unchanged.
 *
 * Alarm (bed_modes_alarm())
 *   The alarm starts ringing (false -> true): mode NONE, timers dropped,
 *   candles off, no command (the speaker stops the music itself when it
 *   starts ringing). While ringing, all buttons are ignored. ringing ->
 *   false changes nothing. Repeating the same value changes nothing.
 */
#ifndef BED_MODES_H
#define BED_MODES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BED_CMDS 8
#define BED_NIGHT_UNKNOWN 0xFFFFFFFFu

typedef enum {
    BED_MODE_NONE = 0,
    BED_MODE_MEDITATION,
    BED_MODE_MOOD,
    BED_MODE_SLEEP,
} bed_mode_t;

typedef enum {
    BED_BTN_MEDITATION = 0,
    BED_BTN_MOOD,
    BED_BTN_SLEEP,
    BED_BTN_NEXT,
    BED_BTN_STOP,
} bed_button_t;

typedef enum {
    BED_SCENE_NORMAL = 0,
    BED_SCENE_MEDITATION,
    BED_SCENE_MOOD,
    BED_SCENE_SLEEP,
} bed_scene_t;

typedef enum {
    BED_CMD_SCENE = 1,
    BED_CMD_PLAY,
    BED_CMD_STOP,
    BED_CMD_NEXT,
    BED_CMD_MOOD_FOLDER_NEXT,
    BED_CMD_SLEEP_FOLDER_NEXT,
} bed_cmd_type_t;

typedef struct {
    bed_cmd_type_t type;
    bed_scene_t scene;  /* SCENE */
    bed_mode_t folder;  /* PLAY: whose folder */
    bool resume;        /* PLAY */
    bool keep;          /* PLAY */
    uint32_t fade_ms;   /* SCENE, PLAY (fade-in), STOP (fade-out) */
} bed_cmd_t;

typedef struct {
    uint32_t med_ms;          /* 60000 ... 10800000 (3 h)        default 1200000 (20 min) */
    uint32_t med_fade_ms;     /* 0 ... 600000                    default 30000            */
    uint32_t sleep_ms;        /* 0 (no timer) ... 14400000 (4 h) default 1800000 (30 min) */
    uint32_t sleep_fade_ms;   /* 0 ... 600000                    default 120000           */
    uint32_t sleep_candle_ms; /* 0 ... 7200000 (2 h)             default 300000 (5 min)   */
    uint32_t start_fade_ms;   /* 0 ... 10000                     default 3000             */
    uint32_t stop_fade_ms;    /* 0 ... 10000                     default 2000             */
} bed_modes_config_t;

typedef struct {
    bed_mode_t mode;
    bool fading;   /* the mode's closing fade is running */
    bool candles;
} bed_outputs_t;

typedef struct bed_modes bed_modes_t;

void bed_modes_config_default(bed_modes_config_t *cfg);

/* Returns NULL if cfg is NULL or a value is out of range. Starts in
 * BED_MODE_NONE, candles off, no command. saved_night: the night the app saved
 * in NVS, or BED_NIGHT_UNKNOWN if none. */
bed_modes_t *bed_modes_create(const bed_modes_config_t *cfg, uint32_t saved_night, uint32_t now_ms);

/* Accepts NULL. */
void bed_modes_destroy(bed_modes_t *bm);

/* Unknown button values are ignored (false). */
bool bed_modes_button(bed_modes_t *bm, bed_button_t button, uint32_t night, uint32_t now_ms);

void bed_modes_alarm(bed_modes_t *bm, bool ringing, uint32_t now_ms);

/* Call regularly (e.g. every 20 ms). */
void bed_modes_tick(bed_modes_t *bm, uint32_t now_ms);

/* The pointer stays valid for the object's lifetime. */
const bed_outputs_t *bed_modes_outputs(const bed_modes_t *bm);

bool bed_modes_take_cmd(bed_modes_t *bm, bed_cmd_t *out);

/* Night of the last first SLEEP press (see Nights), BED_NIGHT_UNKNOWN if none. */
uint32_t bed_modes_sleep_night(const bed_modes_t *bm);

/* Night of a local time: nights start at 12:00 local time.
 * local_s = seconds since 1970-01-01 00:00 *local* time (timegm() of the local
 * broken-down time). Returns (local_s - 43200) / 86400, rounded down; for
 * local_s below 43200 it returns 0. */
uint32_t bed_night_id(int64_t local_s);

/*
 * Mood folder cycling: of the names that start with `prefix` (ASCII, any
 * case), the first that comes after `current` in order, or the first of them
 * if none comes after it. Order: case-insensitive ASCII comparison, ties
 * broken by strcmp(). `current` need not be in the list ("" gives the first).
 * Writes the name to out and returns true; returns false (out = "" if
 * out_size > 0) if no name has the prefix, out is too small for the name,
 * or names, current, prefix or out is NULL. NULL entries in names are skipped.
 */
bool bed_next_folder(const char *const *names, size_t count, const char *current, const char *prefix, char *out,
                     size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* BED_MODES_H */
