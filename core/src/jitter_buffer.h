/**
 * @file jitter_buffer.h
 * @brief Adaptive jitter buffer for received audio streams.
 *
 * Lock-free SPSC design using atomic operations only -- no mutexes.
 * Network thread pushes decoded frames, audio thread pops them for mixing.
 *
 * Features:
 *   - Adaptive target depth based on observed jitter
 *   - PLC flag when buffer runs empty
 *   - Crossfade support for smooth buffer level adjustments
 */

#ifndef JITTER_BUFFER_H
#define JITTER_BUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "../include/audio_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum frames the jitter buffer can hold */
#define AE_JITTER_MAX_CAPACITY 50  /* 50 * 20ms = 1 second */

/* Number of samples per frame (interleaved stereo) */
#define AE_JITTER_FRAME_SAMPLES (AE_FRAME_SIZE * AE_CHANNELS)

typedef struct {
    /* Circular buffer of decoded audio frames */
    float           frames[AE_JITTER_MAX_CAPACITY][AE_FRAME_SIZE * AE_CHANNELS];

    /* SPSC ring buffer positions (atomic for lock-free operation) */
    atomic_uint     write_pos;      /* next slot to write (network thread) */
    atomic_uint     read_pos;       /* next slot to read  (audio thread)  */

    /* Buffer statistics (updated atomically) */
    atomic_int      fill_level;     /* current number of frames buffered */

    /* Configuration */
    int             target_depth;   /* target buffer depth in frames */
    int             min_depth;      /* minimum depth before PLC kicks in */
    int             max_depth;      /* maximum depth before dropping frames */

    /* PLC state (read by audio thread) */
    atomic_bool     needs_plc;      /* set when buffer underruns */

    /* Crossfade buffer for smooth transitions */
    float           crossfade_buf[AE_FRAME_SIZE * AE_CHANNELS];
    atomic_bool     crossfade_pending;

    /* Sequence tracking */
    atomic_uint     last_seq;       /* last sequence number pushed */
    atomic_uint     total_pushed;   /* total frames pushed */
    atomic_uint     total_popped;   /* total frames popped */
    atomic_uint     underruns;      /* underrun count */
} ae_jitter_t;

/**
 * Create a jitter buffer with the given target depth.
 * @param target_depth_frames  Target buffer fill level in frames (e.g. 3 = 60ms).
 * @param min_depth_frames     Minimum depth in frames (default 2).
 * @param max_depth_frames     Maximum depth in frames (default 10).
 * @return Allocated jitter buffer, or NULL on failure.
 */
ae_jitter_t *ae_jitter_create(int target_depth_frames,
                              int min_depth_frames,
                              int max_depth_frames);

/** Destroy a jitter buffer. */
void ae_jitter_destroy(ae_jitter_t *jb);

/**
 * Push a decoded audio frame into the jitter buffer (network thread).
 * @param jb     Jitter buffer.
 * @param frame  Interleaved float32 stereo frame (AE_FRAME_SIZE * AE_CHANNELS).
 * @param seq    Sequence number for reordering detection.
 * @return 0 on success, -1 if buffer is full (frame dropped).
 */
int ae_jitter_push(ae_jitter_t *jb, const float *frame, uint32_t seq);

/**
 * Pop a frame from the jitter buffer for mixing (audio thread).
 * @param jb     Jitter buffer.
 * @param frame  Output: interleaved float32 stereo frame.
 * @return 0 on success, -1 if buffer is empty (caller should use PLC or silence).
 */
int ae_jitter_pop(ae_jitter_t *jb, float *frame);

/**
 * Get the current fill level in frames (thread-safe).
 */
int ae_jitter_level(const ae_jitter_t *jb);

/**
 * Check if PLC is needed (buffer ran empty on last pop attempt).
 */
bool ae_jitter_needs_plc(const ae_jitter_t *jb);

/**
 * Reset the jitter buffer to empty state. NOT thread-safe.
 */
void ae_jitter_reset(ae_jitter_t *jb);

/**
 * Get underrun count (thread-safe).
 */
uint32_t ae_jitter_underruns(const ae_jitter_t *jb);

#ifdef __cplusplus
}
#endif

#endif /* JITTER_BUFFER_H */
