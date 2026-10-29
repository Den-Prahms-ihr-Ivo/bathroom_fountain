/*
 * clap_replay – runs YOUR lib/mic_frontend and lib/clap_detector over a raw
 * sample stream, exactly as the firmware will. Built and driven by
 * tools/clap_lab.py; you normally don't call it directly.
 *
 * stdin   raw little-endian int32 I2S words (24-bit sample << 8)
 * stdout  one line per detected double clap:  E <first> <second> <confirmed>
 *         (sample indices), then a final line:  N <total samples>
 *
 * Options (settings names, see lib/settings/settings.h):
 *   --set clap_onset_db=10.5   (repeatable)
 *   --rate 16000
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "clap_detector.h"
#include "mic_frontend.h"

#define BLOCK 512

static int apply(clap_config_t *cc, mic_frontend_config_t *fc, const char *kv)
{
    char key[64];
    const char *eq = strchr(kv, '=');
    if (eq == NULL || (size_t)(eq - kv) >= sizeof(key)) {
        return 0;
    }
    memcpy(key, kv, (size_t)(eq - kv));
    key[eq - kv] = '\0';
    const char *v = eq + 1;
    char *end = NULL;
    const double d = strtod(v, &end);
    if (end == v || *end != '\0') {
        return 0;
    }
    if (strcmp(key, "clap_min_gap_ms") == 0) {
        cc->min_gap_ms = (uint16_t)d;
    } else if (strcmp(key, "clap_max_gap_ms") == 0) {
        cc->max_gap_ms = (uint16_t)d;
    } else if (strcmp(key, "clap_level_diff_db") == 0) {
        cc->max_level_diff_db = (float)d;
    } else if (strcmp(key, "clap_quiet_before_ms") == 0) {
        cc->quiet_before_ms = (uint16_t)d;
    } else if (strcmp(key, "clap_quiet_after_ms") == 0) {
        cc->quiet_after_ms = (uint16_t)d;
    } else if (strcmp(key, "clap_gap_factor") == 0) {
        cc->quiet_gap_factor = (float)d;
    } else if (strcmp(key, "clap_drop_db") == 0) {
        cc->min_drop_db = (float)d;
    } else if (strcmp(key, "clap_onset_db") == 0) {
        cc->onset_threshold_db = (float)d;
    } else if (strcmp(key, "clap_min_peak_dbfs") == 0) {
        cc->min_peak_dbfs = (float)d;
    } else if (strcmp(key, "mic_hp_hz") == 0) {
        fc->hp_cutoff_hz = (float)d;
    } else if (strcmp(key, "mic_shift") == 0) {
        fc->shift = (uint8_t)d;
    } else {
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    clap_config_t cc;
    mic_frontend_config_t fc;
    clap_config_default(&cc);
    mic_frontend_config_default(&fc);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--set") == 0 && i + 1 < argc) {
            if (!apply(&cc, &fc, argv[++i])) {
                fprintf(stderr, "clap_replay: unknown or malformed setting '%s'\n", argv[i]);
                return 2;
            }
        } else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            cc.sample_rate_hz = (uint32_t)strtoul(argv[++i], NULL, 10);
            fc.sample_rate_hz = cc.sample_rate_hz;
        } else {
            fprintf(stderr, "usage: clap_replay [--rate HZ] [--set key=value]... < words.raw\n");
            return 2;
        }
    }

    mic_frontend_t *mf = mic_frontend_create(&fc);
    clap_detector_t *det = clap_detector_create(&cc);
    if (mf == NULL || det == NULL) {
        fprintf(stderr, "clap_replay: create failed (invalid settings, or module not implemented yet)\n");
        return 3;
    }

    static int32_t words[BLOCK];
    static int16_t pcm[BLOCK];
    clap_event_t ev[8];
    unsigned long long total = 0;
    size_t got;
    while ((got = fread(words, sizeof(int32_t), BLOCK, stdin)) > 0) {
        mic_frontend_process(mf, words, got, pcm);
        const size_t k = clap_detector_process(det, pcm, got, ev, 8);
        for (size_t j = 0; j < k; j++) {
            printf("E %llu %llu %llu\n", (unsigned long long)ev[j].first_onset_sample,
                   (unsigned long long)ev[j].second_onset_sample, (unsigned long long)ev[j].confirmed_sample);
        }
        total += got;
    }
    printf("N %llu\n", total);
    clap_detector_destroy(det);
    mic_frontend_destroy(mf);
    return 0;
}
