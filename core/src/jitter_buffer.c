/**
 * @file jitter_buffer.c
 * @brief Adaptive jitter buffer implementation.
 *
 * Lock-free SPSC ring buffer using atomic operations only.
 * Supports crossfade on buffer level adjustments.
 */

#include "jitter_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

ae_jitter_t *ae_jitter_create(int target_depth_frames,
                              int min_depth_frames,
                              int max_depth_frames)
{
    if (target_depth_frames <= 0)
        target_depth_frames = 3;
    if (min_depth_frames <= 0)
        min_depth_frames = 2;
    if (max_depth_frames <= 0)
        max_depth_frames = AE_JITTER_BUF_FRAMES;
    if (max_depth_frames > AE_JITTER_MAX_CAPACITY)
        max_depth_frames = AE_JITTER_MAX_CAPACITY;

    ae_jitter_t *jb = (ae_jitter_t *)calloc(1, sizeof(ae_jitter_t));
    if (!jb)
        return NULL;

    jb->target_depth = target_depth_frames;
    jb->min_depth    = min_depth_frames;
    jb->max_depth    = max_depth_frames;

    atomic_init(&jb->write_pos, 0);
    atomic_init(&jb->read_pos, 0);
    atomic_init(&jb->fill_level, 0);
    atomic_init(&jb->needs_plc, false);
    atomic_init(&jb->crossfade_pending, false);
    atomic_init(&jb->last_seq, 0);
    atomic_init(&jb->total_pushed, 0);
    atomic_init(&jb->total_popped, 0);
    atomic_init(&jb->underruns, 0);

    return jb;
}

void ae_jitter_destroy(ae_jitter_t *jb)
{
    free(jb);
}

int ae_jitter_push(ae_jitter_t *jb, const float *frame, uint32_t seq)
{
    if (!jb || !frame)
        return -1;

    int level = atomic_load_explicit(&jb->fill_level, memory_order_acquire);

    /* If buffer is full, drop the frame */
    if (level >= jb->max_depth)
        return -1;

    unsigned int w = atomic_load_explicit(&jb->write_pos, memory_order_relaxed);
    unsigned int slot = w % AE_JITTER_MAX_CAPACITY;

    /* Copy the frame into the slot */
    memcpy(jb->frames[slot], frame,
           AE_JITTER_FRAME_SAMPLES * sizeof(float));

    /* Advance write position */
    atomic_store_explicit(&jb->write_pos, w + 1, memory_order_release);
    atomic_fetch_add_explicit(&jb->fill_level, 1, memory_order_release);

    /* Track sequence */
    atomic_store_explicit(&jb->last_seq, seq, memory_order_relaxed);
    atomic_fetch_add_explicit(&jb->total_pushed, 1, memory_order_relaxed);

    /* Clear PLC flag since we have data now */
    atomic_store_explicit(&jb->needs_plc, false, memory_order_relaxed);

    return 0;
}

int ae_jitter_pop(ae_jitter_t *jb, float *frame)
{
    if (!jb || !frame)
        return -1;

    int level = atomic_load_explicit(&jb->fill_level, memory_order_acquire);

    if (level <= 0) {
        /* Buffer empty -- signal that PLC is needed */
        atomic_store_explicit(&jb->needs_plc, true, memory_order_relaxed);
        atomic_fetch_add_explicit(&jb->underruns, 1, memory_order_relaxed);
        return -1;
    }

    unsigned int r = atomic_load_explicit(&jb->read_pos, memory_order_relaxed);
    unsigned int slot = r % AE_JITTER_MAX_CAPACITY;

    /* Check if crossfade is pending (buffer was adjusted) */
    bool do_crossfade = atomic_exchange_explicit(&jb->crossfade_pending, false,
                                                  memory_order_acquire);

    if (do_crossfade) {
        /* Crossfade between the previous frame (in crossfade_buf) and current */
        const float *src = jb->frames[slot];
        int total = AE_JITTER_FRAME_SAMPLES;

        for (int i = 0; i < total; i++) {
            float t = (float)i / (float)total;  /* 0.0 → 1.0 over the frame */
            frame[i] = jb->crossfade_buf[i] * (1.0f - t) + src[i] * t;
        }
    } else {
        memcpy(frame, jb->frames[slot],
               AE_JITTER_FRAME_SAMPLES * sizeof(float));
    }

    /* Advance read position */
    atomic_store_explicit(&jb->read_pos, r + 1, memory_order_release);
    atomic_fetch_sub_explicit(&jb->fill_level, 1, memory_order_release);
    atomic_fetch_add_explicit(&jb->total_popped, 1, memory_order_relaxed);

    /* If buffer level exceeds max, skip frames to catch up.
       Save the last skipped frame for crossfade. */
    level = atomic_load_explicit(&jb->fill_level, memory_order_acquire);
    if (level > jb->max_depth) {
        int to_skip = level - jb->target_depth;
        for (int i = 0; i < to_skip; i++) {
            unsigned int skip_r = atomic_load_explicit(&jb->read_pos,
                                                       memory_order_relaxed);
            unsigned int skip_slot = skip_r % AE_JITTER_MAX_CAPACITY;

            /* Save last skipped frame for crossfade */
            if (i == to_skip - 1) {
                memcpy(jb->crossfade_buf, jb->frames[skip_slot],
                       AE_JITTER_FRAME_SAMPLES * sizeof(float));
                atomic_store_explicit(&jb->crossfade_pending, true,
                                      memory_order_release);
            }

            atomic_store_explicit(&jb->read_pos, skip_r + 1,
                                  memory_order_release);
            atomic_fetch_sub_explicit(&jb->fill_level, 1,
                                      memory_order_release);
        }
    }

    return 0;
}

int ae_jitter_level(const ae_jitter_t *jb)
{
    if (!jb)
        return 0;
    int level = atomic_load_explicit(&jb->fill_level, memory_order_acquire);
    return level > 0 ? level : 0;
}

bool ae_jitter_needs_plc(const ae_jitter_t *jb)
{
    if (!jb)
        return false;
    return atomic_load_explicit(&jb->needs_plc, memory_order_acquire);
}

void ae_jitter_reset(ae_jitter_t *jb)
{
    if (!jb)
        return;
    atomic_store(&jb->write_pos, 0);
    atomic_store(&jb->read_pos, 0);
    atomic_store(&jb->fill_level, 0);
    atomic_store(&jb->needs_plc, false);
    atomic_store(&jb->crossfade_pending, false);
    atomic_store(&jb->total_pushed, 0);
    atomic_store(&jb->total_popped, 0);
    atomic_store(&jb->underruns, 0);
}

uint32_t ae_jitter_underruns(const ae_jitter_t *jb)
{
    if (!jb)
        return 0;
    return atomic_load_explicit(&jb->underruns, memory_order_relaxed);
}
