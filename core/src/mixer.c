/**
 * @file mixer.c
 * @brief Float32 audio mixer implementation.
 *
 * All processing in float32.  Uses tanhf() soft limiter.
 * Per-input volume with smooth 5ms linear ramp.
 */

#include "mixer.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct ae_mixer {
    int   max_inputs;
    float master_volume;        /* current (ramped) master volume */
    float master_volume_target; /* target master volume */

    /* Per-input previous volumes for ramping */
    float prev_volumes[AE_MIXER_MAX_INPUTS];
};

ae_mixer_t *ae_mixer_create(int max_inputs)
{
    if (max_inputs <= 0)
        max_inputs = AE_MIXER_MAX_INPUTS;
    if (max_inputs > AE_MIXER_MAX_INPUTS)
        max_inputs = AE_MIXER_MAX_INPUTS;

    ae_mixer_t *m = (ae_mixer_t *)calloc(1, sizeof(ae_mixer_t));
    if (!m)
        return NULL;

    m->max_inputs = max_inputs;
    m->master_volume = 1.0f;
    m->master_volume_target = 1.0f;

    /* Initialize all previous volumes to 0 so the first frame ramps up */
    memset(m->prev_volumes, 0, sizeof(m->prev_volumes));

    return m;
}

void ae_mixer_destroy(ae_mixer_t *mixer)
{
    free(mixer);
}

/**
 * Soft limiter using tanhf().
 * Maps the full float range to [-1, +1] with gentle saturation.
 * Input near 0 passes through almost linearly; larger values are compressed.
 */
static inline float soft_limit(float sample)
{
    return tanhf(sample);
}

void ae_mixer_mix(ae_mixer_t *mixer,
                  const float *const *inputs,
                  const float *volumes,
                  int n_inputs,
                  float *output)
{
    if (!mixer || !output) return;

    const int total_samples = AE_FRAME_SIZE * AE_CHANNELS;

    /* Clear output buffer */
    memset(output, 0, (size_t)total_samples * sizeof(float));

    if (!inputs || n_inputs <= 0)
        return;

    if (n_inputs > mixer->max_inputs)
        n_inputs = mixer->max_inputs;

    /*
     * Ramp length in samples (interleaved).
     * 5ms at 48kHz = 240 samples per channel = 480 interleaved samples.
     */
    const int ramp_len = AE_RAMP_SAMPLES * AE_CHANNELS;

    /* Mix each input with volume ramping */
    for (int i = 0; i < n_inputs; i++) {
        if (!inputs[i])
            continue;

        float vol_prev = (i < AE_MIXER_MAX_INPUTS) ? mixer->prev_volumes[i] : 0.0f;
        float vol_target = volumes ? volumes[i] : 1.0f;

        /* Clamp volume to [0, 1] */
        if (vol_target < 0.0f) vol_target = 0.0f;
        if (vol_target > 1.0f) vol_target = 1.0f;

        if (fabsf(vol_prev - vol_target) < 1e-6f) {
            /* No ramp needed -- constant volume */
            for (int s = 0; s < total_samples; s++) {
                output[s] += inputs[i][s] * vol_target;
            }
        } else {
            /* Ramp from prev to target over ramp_len, then hold target */
            for (int s = 0; s < total_samples; s++) {
                float vol;
                if (s < ramp_len) {
                    float t = (float)s / (float)ramp_len;
                    vol = vol_prev + (vol_target - vol_prev) * t;
                } else {
                    vol = vol_target;
                }
                output[s] += inputs[i][s] * vol;
            }
        }

        /* Store current volume for next frame's ramp */
        if (i < AE_MIXER_MAX_INPUTS)
            mixer->prev_volumes[i] = vol_target;
    }

    /* Apply master volume with ramping */
    float mv_prev   = mixer->master_volume;
    float mv_target = mixer->master_volume_target;

    if (fabsf(mv_prev - mv_target) < 1e-6f) {
        /* No ramp -- constant master volume */
        if (fabsf(mv_target - 1.0f) > 1e-6f) {
            for (int s = 0; s < total_samples; s++) {
                output[s] *= mv_target;
            }
        }
    } else {
        for (int s = 0; s < total_samples; s++) {
            float vol;
            if (s < ramp_len) {
                float t = (float)s / (float)ramp_len;
                vol = mv_prev + (mv_target - mv_prev) * t;
            } else {
                vol = mv_target;
            }
            output[s] *= vol;
        }
    }
    mixer->master_volume = mv_target;

    /* Apply soft limiter to every sample */
    for (int s = 0; s < total_samples; s++) {
        output[s] = soft_limit(output[s]);
    }
}

void ae_mixer_set_master_volume(ae_mixer_t *mixer, float volume)
{
    if (!mixer) return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    mixer->master_volume_target = volume;
}

float ae_mixer_get_master_volume(const ae_mixer_t *mixer)
{
    if (!mixer) return 0.0f;
    return mixer->master_volume_target;
}
