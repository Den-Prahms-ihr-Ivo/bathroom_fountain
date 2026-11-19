/*
 * voice_mix – a mono voice clip mixed over stereo music, with ducking.
 * Contract in voice_mix.h, tests in test/test_voice_mix/.
 *
 * The clip position is s0 + acc / out_rate between the clip samples s0 and
 * s1; each output frame adds clip_rate to acc. Everything is decided per
 * frame, so the block size never changes the result.
 */
#include "voice_mix.h"

#include <math.h>
#include <stdlib.h>

#define RATE_MIN 8000u
#define RATE_MAX 48000u

struct voice_mix {
    voice_mix_config_t cfg;
    uint32_t out_rate;
    float floor_gain; /* 10^(-duck_db/20) */
    float voice_gain; /* 10^(voice_gain_db/20) */
    float duck;       /* current music gain */

    /* clip */
    bool active;
    bool primed;
    bool source_done;
    voice_mix_read_fn read;
    void *ctx;
    uint32_t clip_rate;
    uint32_t acc;
    int16_t s0;
    int16_t s1;
    bool has_s1;
    int16_t buf[VOICE_MIX_READ_MAX];
    size_t buf_len;
    size_t buf_pos;
};

static bool rate_ok(uint32_t r)
{
    return r >= RATE_MIN && r <= RATE_MAX;
}

void voice_mix_config_default(voice_mix_config_t *cfg)
{
    if (cfg != NULL) {
        cfg->out_rate_hz = 48000;
        cfg->duck_db = 12.0f;
        cfg->duck_ramp_ms = 60;
        cfg->voice_gain_db = 0.0f;
    }
}

voice_mix_t *voice_mix_create(const voice_mix_config_t *cfg)
{
    if (cfg == NULL || !rate_ok(cfg->out_rate_hz) || !(cfg->duck_db >= 0.0f && cfg->duck_db <= 40.0f) ||
        cfg->duck_ramp_ms > 1000 || !(cfg->voice_gain_db >= -20.0f && cfg->voice_gain_db <= 12.0f)) {
        return NULL;
    }
    voice_mix_t *vm = calloc(1, sizeof(*vm));
    if (vm == NULL) {
        return NULL;
    }
    vm->cfg = *cfg;
    vm->out_rate = cfg->out_rate_hz;
    vm->floor_gain = powf(10.0f, -cfg->duck_db / 20.0f);
    vm->voice_gain = powf(10.0f, cfg->voice_gain_db / 20.0f);
    vm->duck = 1.0f;
    return vm;
}

void voice_mix_destroy(voice_mix_t *vm)
{
    free(vm);
}

bool voice_mix_set_out_rate(voice_mix_t *vm, uint32_t out_rate_hz)
{
    if (vm == NULL || !rate_ok(out_rate_hz)) {
        return false;
    }
    vm->acc = (uint32_t)((uint64_t)vm->acc * out_rate_hz / vm->out_rate);
    vm->out_rate = out_rate_hz;
    return true;
}

bool voice_mix_start(voice_mix_t *vm, uint32_t clip_rate_hz, voice_mix_read_fn read, void *ctx)
{
    if (vm == NULL || read == NULL || !rate_ok(clip_rate_hz)) {
        return false;
    }
    vm->read = read;
    vm->ctx = ctx;
    vm->clip_rate = clip_rate_hz;
    vm->active = true;
    vm->primed = false;
    vm->source_done = false;
    vm->buf_len = 0;
    vm->buf_pos = 0;
    vm->acc = 0;
    vm->has_s1 = false;
    return true;
}

void voice_mix_stop(voice_mix_t *vm)
{
    if (vm != NULL) {
        vm->active = false;
    }
}

bool voice_mix_active(const voice_mix_t *vm)
{
    return vm != NULL && vm->active;
}

static bool next_sample(voice_mix_t *vm, int16_t *out)
{
    if (vm->buf_pos >= vm->buf_len) {
        if (vm->source_done) {
            return false;
        }
        const size_t n = vm->read(vm->ctx, vm->buf, VOICE_MIX_READ_MAX);
        if (n == 0) {
            vm->source_done = true;
            return false;
        }
        vm->buf_len = n > VOICE_MIX_READ_MAX ? VOICE_MIX_READ_MAX : n;
        vm->buf_pos = 0;
    }
    *out = vm->buf[vm->buf_pos++];
    return true;
}

static void prime(voice_mix_t *vm)
{
    vm->primed = true;
    if (!next_sample(vm, &vm->s0)) {
        vm->active = false;
        return;
    }
    vm->has_s1 = next_sample(vm, &vm->s1);
    vm->acc = 0;
}

/* Move the clip on by one output frame; ends the clip after its last sample. */
static void advance(voice_mix_t *vm)
{
    vm->acc += vm->clip_rate;
    while (vm->acc >= vm->out_rate) {
        vm->acc -= vm->out_rate;
        if (!vm->has_s1) {
            vm->active = false;
            return;
        }
        vm->s0 = vm->s1;
        vm->has_s1 = next_sample(vm, &vm->s1);
    }
}

static int16_t saturate(float v)
{
    const float r = roundf(v);
    if (r > 32767.0f) {
        return 32767;
    }
    if (r < -32768.0f) {
        return -32768;
    }
    return (int16_t)r;
}

void voice_mix_process(voice_mix_t *vm, int16_t *stereo, size_t frames)
{
    if (vm == NULL || stereo == NULL) {
        return;
    }
    for (size_t f = 0; f < frames; f++) {
        if (vm->active && !vm->primed) {
            prime(vm);
        }
        const bool voiced = vm->active;
        const float target = voiced ? vm->floor_gain : 1.0f;
        const float full_frames = (float)vm->cfg.duck_ramp_ms * (float)vm->out_rate / 1000.0f;
        if (full_frames < 1.0f) {
            vm->duck = target;
        }

        if (voiced || vm->duck < 1.0f) {
            float voice = 0.0f;
            if (voiced) {
                voice = vm->s0;
                if (vm->has_s1 && vm->acc != 0) {
                    voice += (float)(vm->s1 - vm->s0) * (float)vm->acc / (float)vm->out_rate;
                }
                voice *= vm->voice_gain;
            }
            int16_t *frame = stereo + 2 * f;
            for (int ch = 0; ch < 2; ch++) {
                const float m = vm->duck == 1.0f ? (float)frame[ch] : roundf((float)frame[ch] * vm->duck);
                frame[ch] = saturate(m + voice);
            }
        }

        if (full_frames >= 1.0f && vm->duck != target) {
            const float step = (1.0f - vm->floor_gain) / full_frames;
            if (vm->duck > target) {
                vm->duck -= step;
                if (vm->duck < target) {
                    vm->duck = target;
                }
            } else {
                vm->duck += step;
                if (vm->duck > target) {
                    vm->duck = target;
                }
            }
        }

        if (voiced) {
            advance(vm);
        }
    }
}
