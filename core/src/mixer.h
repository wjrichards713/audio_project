/**
 * @file mixer.h
 * @brief Float32 audio mixer with soft limiting and volume ramping.
 *
 * All operations in float32 -- never converts to int16.
 *
 * Features:
 *   - Mix N input buffers into one output
 *   - Per-channel volume with smooth 5ms cosine ramp
 *   - Master volume control
 *   - Soft limiter using tanhf() to prevent clipping
 *   - Processes AE_FRAME_SIZE (960) samples at a time
 */

#ifndef MIXER_H
#define MIXER_H

#include <stdint.h>
#include <stdbool.h>
#include "../include/audio_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 5ms ramp at 48kHz = 240 samples per channel */
#define AE_RAMP_SAMPLES   240

/* Maximum number of inputs that can be mixed simultaneously */
#define AE_MIXER_MAX_INPUTS  AE_MAX_USERS_PER_CH

typedef struct ae_mixer ae_mixer_t;

/**
 * Create a mixer.
 * @param max_inputs  Maximum number of simultaneous input streams.
 * @return Mixer handle, or NULL on failure.
 */
ae_mixer_t *ae_mixer_create(int max_inputs);

/** Destroy a mixer. */
void ae_mixer_destroy(ae_mixer_t *mixer);

/**
 * Mix multiple input buffers into a single output buffer.
 *
 * Each input buffer contains AE_FRAME_SIZE * AE_CHANNELS interleaved
 * float32 samples.  The mixer applies per-input volume, sums all inputs,
 * applies master volume, and then a soft limiter.
 *
 * @param mixer        Mixer handle.
 * @param inputs       Array of pointers to input frame buffers.
 * @param volumes      Per-input volume [0.0 .. 1.0], array of n_inputs.
 * @param n_inputs     Number of input buffers.
 * @param output       Output buffer (AE_FRAME_SIZE * AE_CHANNELS floats).
 */
void ae_mixer_mix(ae_mixer_t *mixer,
                  const float *const *inputs,
                  const float *volumes,
                  int n_inputs,
                  float *output);

/**
 * Set master output volume [0.0 .. 1.0].
 * The change is applied with smooth ramping on the next mix call.
 */
void ae_mixer_set_master_volume(ae_mixer_t *mixer, float volume);

/**
 * Get current master volume.
 */
float ae_mixer_get_master_volume(const ae_mixer_t *mixer);

#ifdef __cplusplus
}
#endif

#endif /* MIXER_H */
