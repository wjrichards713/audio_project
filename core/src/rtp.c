/**
 * @file rtp.c
 * @brief RTP-like packet building and parsing implementation.
 */

#include "rtp.h"
#include <string.h>

/* ─── Byte-order helpers (always big-endian on wire) ──────────────── */

static inline void write_u16_be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

static inline void write_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)(v);
}

static inline void write_u64_be(uint8_t *p, uint64_t v)
{
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >>  8);
    p[7] = (uint8_t)(v);
}

static inline uint16_t read_u16_be(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static inline uint32_t read_u32_be(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] <<  8 | (uint32_t)p[3];
}

static inline uint64_t read_u64_be(const uint8_t *p)
{
    return (uint64_t)p[0] << 56 | (uint64_t)p[1] << 48 |
           (uint64_t)p[2] << 40 | (uint64_t)p[3] << 32 |
           (uint64_t)p[4] << 24 | (uint64_t)p[5] << 16 |
           (uint64_t)p[6] <<  8 | (uint64_t)p[7];
}

/* ─── Routing header serialization ────────────────────────────────── */

static void serialize_header(const ae_rtp_header_t *h, uint8_t *buf)
{
    write_u32_be(buf + 0,  h->server_id);
    write_u32_be(buf + 4,  h->channel_id);
    write_u64_be(buf + 8,  h->client_id);
    buf[16] = h->packet_type;
    buf[17] = h->flags;
    write_u16_be(buf + 18, h->payload_length);
}

/* ─── Public API ──────────────────────────────────────────────────── */

void ae_rtp_seq_init(ae_rtp_seq_tracker_t *tracker)
{
    if (!tracker) return;
    memset(tracker, 0, sizeof(*tracker));
}

int ae_rtp_seq_update(ae_rtp_seq_tracker_t *tracker, uint32_t seq)
{
    if (!tracker) return -1;

    tracker->received++;

    if (tracker->received == 1) {
        /* First packet -- initialize */
        tracker->next_seq = seq + 1;
        tracker->max_seq  = seq;
        return 0;
    }

    if (seq == tracker->next_seq) {
        /* In order */
        tracker->next_seq = seq + 1;
        if (seq > tracker->max_seq)
            tracker->max_seq = seq;
        return 0;
    }

    if (seq > tracker->next_seq) {
        /* Gap detected -- packets lost */
        uint32_t gap = seq - tracker->next_seq;
        tracker->lost += gap;
        tracker->next_seq = seq + 1;
        tracker->max_seq  = seq;
        return 0;
    }

    /* seq < next_seq -- either reordered or duplicate */
    if (seq > tracker->max_seq - 1000) {
        /* Likely reordered (within recent window) */
        tracker->reordered++;
        if (tracker->lost > 0)
            tracker->lost--;  /* one fewer "lost" since it arrived late */
        return 1;
    }

    /* Too old -- treat as duplicate */
    return -1;
}

int ae_rtp_build_packet(const ae_rtp_header_t *header,
                        const uint8_t *payload, int payload_len,
                        uint8_t *out, int out_size)
{
    if (!header || !out)
        return -1;

    int total = AE_RTP_HEADER_SIZE + payload_len;
    if (out_size < total || total > AE_RTP_MAX_PACKET)
        return -1;

    /* Build header with correct payload length */
    ae_rtp_header_t h = *header;
    h.payload_length = (uint16_t)payload_len;
    serialize_header(&h, out);

    /* Copy payload */
    if (payload && payload_len > 0)
        memcpy(out + AE_RTP_HEADER_SIZE, payload, (size_t)payload_len);

    return total;
}

int ae_rtp_parse_header(const uint8_t *data, int data_len,
                        ae_rtp_header_t *header)
{
    if (!data || !header || data_len < AE_RTP_HEADER_SIZE)
        return -1;

    header->server_id      = read_u32_be(data + 0);
    header->channel_id     = read_u32_be(data + 4);
    header->client_id      = read_u64_be(data + 8);
    header->packet_type    = data[16];
    header->flags          = data[17];
    header->payload_length = read_u16_be(data + 18);

    return 0;
}

int ae_rtp_build_keepalive(uint32_t server_id, uint32_t channel_id,
                           uint64_t client_id,
                           uint8_t *out, int out_size)
{
    ae_rtp_header_t h = {0};
    h.server_id   = server_id;
    h.channel_id  = channel_id;
    h.client_id   = client_id;
    h.packet_type = AE_RTP_TYPE_KEEPALIVE;
    h.flags       = 0;
    h.payload_length = 0;

    return ae_rtp_build_packet(&h, NULL, 0, out, out_size);
}

int ae_rtp_build_ping(uint32_t server_id, uint64_t client_id,
                      uint64_t timestamp,
                      uint8_t *out, int out_size)
{
    ae_rtp_header_t h = {0};
    h.server_id   = server_id;
    h.channel_id  = 0;
    h.client_id   = client_id;
    h.packet_type = AE_RTP_TYPE_PING;
    h.flags       = 0;

    /* Payload: 8-byte timestamp */
    uint8_t payload[8];
    write_u64_be(payload, timestamp);

    return ae_rtp_build_packet(&h, payload, 8, out, out_size);
}

const uint8_t *ae_rtp_get_payload(const uint8_t *data, int data_len)
{
    if (!data || data_len <= AE_RTP_HEADER_SIZE)
        return NULL;
    return data + AE_RTP_HEADER_SIZE;
}
