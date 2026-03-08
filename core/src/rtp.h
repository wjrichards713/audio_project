/**
 * @file rtp.h
 * @brief RTP-like packet building and parsing.
 *
 * Packet layout:
 *   routing_header(20 bytes) + encrypted_payload(variable)
 *
 * Routing header (20 bytes, all network byte order / big-endian):
 *   Bytes  0- 3: server_id    (uint32_t)
 *   Bytes  4- 7: channel_id   (uint32_t)
 *   Bytes  8-15: client_id    (uint64_t)
 *   Byte     16: packet_type  (uint8_t)  0=audio, 1=keepalive, 2=ping, 3=pong
 *   Byte     17: flags        (uint8_t)
 *   Bytes 18-19: payload_length (uint16_t)
 */

#ifndef RTP_H
#define RTP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AE_RTP_HEADER_SIZE  20
#define AE_RTP_MAX_PACKET   1500

/* Packet types */
#define AE_RTP_TYPE_AUDIO     0
#define AE_RTP_TYPE_KEEPALIVE 1
#define AE_RTP_TYPE_PING      2
#define AE_RTP_TYPE_PONG      3

/* Flags */
#define AE_RTP_FLAG_STEREO    0x01
#define AE_RTP_FLAG_FEC       0x02

/* ─── Routing header struct ───────────────────────────────────────── */

typedef struct {
    uint32_t server_id;
    uint32_t channel_id;
    uint64_t client_id;
    uint8_t  packet_type;
    uint8_t  flags;
    uint16_t payload_length;
} ae_rtp_header_t;

/* ─── Sequence tracker for reordering and loss detection ──────────── */

typedef struct {
    uint32_t next_seq;         /* expected next sequence number */
    uint32_t max_seq;          /* highest sequence seen */
    uint32_t received;         /* packets received */
    uint32_t lost;             /* packets detected as lost */
    uint32_t reordered;        /* packets received out of order */
} ae_rtp_seq_tracker_t;

/**
 * Initialize a sequence tracker.
 */
void ae_rtp_seq_init(ae_rtp_seq_tracker_t *tracker);

/**
 * Update the sequence tracker with a received sequence number.
 * @return 0 if in-order, 1 if out-of-order, -1 if duplicate.
 */
int ae_rtp_seq_update(ae_rtp_seq_tracker_t *tracker, uint32_t seq);

/**
 * Build a packet with routing header + payload.
 *
 * @param header   Routing header fields (payload_length is set automatically).
 * @param payload  Encrypted payload data.
 * @param payload_len  Length of payload in bytes.
 * @param out      Output buffer (must be >= AE_RTP_HEADER_SIZE + payload_len).
 * @param out_size Size of output buffer.
 * @return Total packet size, or -1 on error.
 */
int ae_rtp_build_packet(const ae_rtp_header_t *header,
                        const uint8_t *payload, int payload_len,
                        uint8_t *out, int out_size);

/**
 * Parse the routing header from a received packet.
 *
 * @param data     Raw packet data.
 * @param data_len Length of packet.
 * @param header   Output: parsed header fields.
 * @return 0 on success, -1 if packet too short.
 */
int ae_rtp_parse_header(const uint8_t *data, int data_len,
                        ae_rtp_header_t *header);

/**
 * Build a keepalive packet.
 *
 * @param server_id  Server ID.
 * @param channel_id Channel ID.
 * @param client_id  Client ID.
 * @param out        Output buffer (>= AE_RTP_HEADER_SIZE bytes).
 * @param out_size   Size of output buffer.
 * @return Packet size, or -1 on error.
 */
int ae_rtp_build_keepalive(uint32_t server_id, uint32_t channel_id,
                           uint64_t client_id,
                           uint8_t *out, int out_size);

/**
 * Build a ping packet with a timestamp payload.
 */
int ae_rtp_build_ping(uint32_t server_id, uint64_t client_id,
                      uint64_t timestamp,
                      uint8_t *out, int out_size);

/**
 * Get a pointer to the payload portion of a parsed packet.
 * @param data      Full packet data.
 * @param data_len  Full packet length.
 * @return Pointer to payload, or NULL if no payload.
 */
const uint8_t *ae_rtp_get_payload(const uint8_t *data, int data_len);

#ifdef __cplusplus
}
#endif

#endif /* RTP_H */
