/**
 * @file ring_buffer.h
 * @brief Lock-free single-producer single-consumer (SPSC) ring buffer.
 *
 * Uses atomic load/store operations only -- no mutexes.
 * Safe for one writer thread and one reader thread operating concurrently.
 *
 * Stores interleaved float32 stereo audio frames.
 */

#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdint.h>
#include <stddef.h>
#include "atomic_compat.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float          *buffer;         /* pre-allocated sample storage */
    int             capacity;       /* total frames (not samples) */
    int             channels;       /* samples per frame */
    atomic_uint     write_pos;      /* next frame to write (producer) */
    atomic_uint     read_pos;       /* next frame to read  (consumer) */
} ae_ringbuf_t;

/**
 * Create a ring buffer.
 * @param capacity  Number of frames the buffer can hold.
 * @param channels  Number of channels (samples per frame).
 * @return Allocated ring buffer, or NULL on failure.
 */
ae_ringbuf_t *ae_ringbuf_create(int capacity, int channels);

/** Destroy a ring buffer and free its memory. */
void ae_ringbuf_destroy(ae_ringbuf_t *rb);

/**
 * Write frames into the ring buffer (producer side).
 * @param rb       Ring buffer.
 * @param data     Interleaved float samples to write.
 * @param frames   Number of frames to write.
 * @return Number of frames actually written (may be less if buffer is full).
 */
int ae_ringbuf_write(ae_ringbuf_t *rb, const float *data, int frames);

/**
 * Read frames from the ring buffer (consumer side).
 * @param rb       Ring buffer.
 * @param data     Output buffer for interleaved float samples.
 * @param frames   Number of frames to read.
 * @return Number of frames actually read (may be less if buffer doesn't have enough).
 */
int ae_ringbuf_read(ae_ringbuf_t *rb, float *data, int frames);

/**
 * Number of frames available for reading.
 */
int ae_ringbuf_available(const ae_ringbuf_t *rb);

/**
 * Number of frames of free space for writing.
 */
int ae_ringbuf_free_space(const ae_ringbuf_t *rb);

/**
 * Reset the buffer to empty state. NOT thread-safe -- call only
 * when neither producer nor consumer is active.
 */
void ae_ringbuf_reset(ae_ringbuf_t *rb);

#ifdef __cplusplus
}
#endif

#endif /* RING_BUFFER_H */
