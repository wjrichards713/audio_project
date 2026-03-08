/**
 * @file ring_buffer.c
 * @brief Lock-free SPSC ring buffer implementation.
 *
 * The ring buffer uses a power-of-two-sized internal array with atomic
 * read/write positions.  Masking is used instead of modulo for efficiency.
 * One slot is always left empty to distinguish full from empty.
 */

#include "ring_buffer.h"
#include <stdlib.h>
#include <string.h>

/* Round up to next power of two */
static int next_power_of_two(int v)
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

ae_ringbuf_t *ae_ringbuf_create(int capacity, int channels)
{
    if (capacity <= 0 || channels <= 0)
        return NULL;

    /* Round capacity up to power of two + 1 extra slot for full/empty distinction */
    int actual_cap = next_power_of_two(capacity + 1);

    ae_ringbuf_t *rb = (ae_ringbuf_t *)calloc(1, sizeof(ae_ringbuf_t));
    if (!rb)
        return NULL;

    rb->buffer = (float *)calloc((size_t)actual_cap * channels, sizeof(float));
    if (!rb->buffer) {
        free(rb);
        return NULL;
    }

    rb->capacity = actual_cap;
    rb->channels = channels;
    atomic_init(&rb->write_pos, 0);
    atomic_init(&rb->read_pos, 0);

    return rb;
}

void ae_ringbuf_destroy(ae_ringbuf_t *rb)
{
    if (!rb)
        return;
    free(rb->buffer);
    free(rb);
}

int ae_ringbuf_available(const ae_ringbuf_t *rb)
{
    if (!rb)
        return 0;
    unsigned int w = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
    unsigned int r = atomic_load_explicit(&rb->read_pos, memory_order_acquire);
    return (int)((w - r) & (unsigned int)(rb->capacity - 1));
}

int ae_ringbuf_free_space(const ae_ringbuf_t *rb)
{
    if (!rb)
        return 0;
    /* capacity - 1 because one slot is reserved */
    return (rb->capacity - 1) - ae_ringbuf_available(rb);
}

int ae_ringbuf_write(ae_ringbuf_t *rb, const float *data, int frames)
{
    if (!rb || !data || frames <= 0)
        return 0;

    int avail = ae_ringbuf_free_space(rb);
    if (frames > avail)
        frames = avail;

    if (frames == 0)
        return 0;

    unsigned int w = atomic_load_explicit(&rb->write_pos, memory_order_relaxed);
    unsigned int mask = (unsigned int)(rb->capacity - 1);
    int stride = rb->channels;

    for (int i = 0; i < frames; i++) {
        unsigned int idx = (w + (unsigned int)i) & mask;
        memcpy(&rb->buffer[idx * stride], &data[i * stride],
               (size_t)stride * sizeof(float));
    }

    /* Release fence: ensure all writes to buffer[] are visible before
       the consumer sees the updated write_pos. */
    atomic_store_explicit(&rb->write_pos, (w + (unsigned int)frames) & mask,
                          memory_order_release);
    return frames;
}

int ae_ringbuf_read(ae_ringbuf_t *rb, float *data, int frames)
{
    if (!rb || !data || frames <= 0)
        return 0;

    int avail = ae_ringbuf_available(rb);
    if (frames > avail)
        frames = avail;

    if (frames == 0)
        return 0;

    unsigned int r = atomic_load_explicit(&rb->read_pos, memory_order_relaxed);
    unsigned int mask = (unsigned int)(rb->capacity - 1);
    int stride = rb->channels;

    for (int i = 0; i < frames; i++) {
        unsigned int idx = (r + (unsigned int)i) & mask;
        memcpy(&data[i * stride], &rb->buffer[idx * stride],
               (size_t)stride * sizeof(float));
    }

    atomic_store_explicit(&rb->read_pos, (r + (unsigned int)frames) & mask,
                          memory_order_release);
    return frames;
}

void ae_ringbuf_reset(ae_ringbuf_t *rb)
{
    if (!rb)
        return;
    atomic_store(&rb->write_pos, 0);
    atomic_store(&rb->read_pos, 0);
}
