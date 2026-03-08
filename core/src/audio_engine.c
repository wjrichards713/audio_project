/**
 * @file audio_engine.c
 * @brief Main audio engine implementation.
 *
 * Orchestrates all subsystems: Opus codec, jitter buffers, mixer,
 * crypto, RTP, and UDP networking.
 *
 * THREADING MODEL:
 *   - Audio thread: ae_engine_process(), ae_engine_write_capture(),
 *     ae_engine_read_playback().  MUST NOT allocate or lock.
 *   - Main/UI thread: all other ae_engine_* calls.
 *   - Lock-free ring buffers bridge the two threads.
 */

#include "../include/audio_engine.h"
#include "ring_buffer.h"
#include "opus_codec.h"
#include "jitter_buffer.h"
#include "mixer.h"
#include "crypto.h"
#include "rtp.h"
#include "network.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

/* ─── Constants ───────────────────────────────────────────────────── */

#define CAPTURE_RING_FRAMES   64   /* ~1.3 seconds of capture buffer */
#define PLAYBACK_RING_FRAMES  64   /* ~1.3 seconds of playback buffer */
#define KEEPALIVE_INTERVAL_MS 5000
#define MAX_RECV_PER_TICK     20   /* max packets to process per tick */

/* ─── Remote stream state ─────────────────────────────────────────── */

typedef struct {
    uint64_t           client_id;
    int                active;
    ae_opus_decoder_t *decoder;
    ae_jitter_t       *jitter;
    ae_rtp_seq_tracker_t seq_tracker;
    float              volume;        /* per-stream volume */
    float              volume_target; /* target volume (for atomic reads) */
} remote_stream_t;

/* ─── Channel state ───────────────────────────────────────────────── */

typedef struct {
    char     channel_id_str[64]; /* string channel ID (from API) */
    uint32_t channel_id_num;     /* numeric channel ID for RTP headers */
    int      active;
    int      transmitting;
    float    volume;
} channel_state_t;

/* ─── Engine internal state ───────────────────────────────────────── */

struct ae_engine {
    /* Configuration (immutable after create) */
    ae_config_t config;

    /* Connection state */
    atomic_bool connected;
    char        server_host[256];
    int         udp_port;

    /* Callbacks */
    ae_capture_callback_t  capture_cb;
    void                  *capture_ud;
    ae_playback_callback_t playback_cb;
    void                  *playback_ud;
    ae_event_callback_t    event_cb;
    void                  *event_ud;

    /* Subsystems */
    ae_opus_encoder_t *encoder;
    ae_net_t          *network;
    ae_crypto_t       *crypto;
    ae_mixer_t        *mixer;

    /* Ring buffers (audio thread <-> engine process) */
    ae_ringbuf_t *capture_ring;    /* platform capture → encode */
    ae_ringbuf_t *playback_ring;   /* decode+mix → platform playback */

    /* Remote streams */
    remote_stream_t streams[AE_MAX_USERS_PER_CH];
    int             stream_count;

    /* Channel state (POC: single channel at a time) */
    channel_state_t channel;

    /* Transmit state */
    atomic_bool transmitting;
    uint32_t    tx_sequence;

    /* Pre-allocated audio thread work buffers (NO malloc in audio path) */
    float    capture_frame[AE_FRAME_SIZE * AE_CHANNELS];
    float    decode_frame[AE_FRAME_SIZE * AE_CHANNELS];
    float    mix_output[AE_FRAME_SIZE * AE_CHANNELS];
    uint8_t  encode_buf[AE_MAX_PACKET_SIZE];
    uint8_t  encrypt_buf[AE_MAX_PACKET_SIZE];
    uint8_t  packet_buf[AE_MAX_PACKET_SIZE];
    uint8_t  recv_buf[AE_MAX_PACKET_SIZE];

    /* Pointers to stream buffers for mixer (pre-allocated) */
    float   *mix_inputs[AE_MAX_USERS_PER_CH];
    float    mix_volumes[AE_MAX_USERS_PER_CH];

    /* Per-stream decode buffers (pre-allocated, one per max stream) */
    float    stream_frames[AE_MAX_USERS_PER_CH][AE_FRAME_SIZE * AE_CHANNELS];

    /* Statistics (atomics for thread-safe reads) */
    atomic_uint_fast64_t packets_sent;
    atomic_uint_fast64_t packets_received;
    atomic_int           buffer_underruns;
    atomic_int           active_streams;
    float                rtt_ms;

    /* Keepalive timer */
    uint64_t last_keepalive_ms;

    /* Master volume */
    float master_volume;

    /* Server/client IDs for packet headers */
    uint32_t server_id;
    uint64_t client_id;
};

/* ─── Helper: parse numeric channel ID from string ────────────────── */

static uint32_t channel_str_to_id(const char *str)
{
    if (!str) return 0;
    /* Simple hash for POC */
    uint32_t h = 5381;
    for (const char *p = str; *p; p++)
        h = ((h << 5) + h) + (uint32_t)*p;
    return h;
}

/* ─── Helper: find or create remote stream ────────────────────────── */

static remote_stream_t *find_stream(ae_engine_t *e, uint64_t client_id)
{
    for (int i = 0; i < AE_MAX_USERS_PER_CH; i++) {
        if (e->streams[i].active && e->streams[i].client_id == client_id)
            return &e->streams[i];
    }
    return NULL;
}

static remote_stream_t *create_stream(ae_engine_t *e, uint64_t client_id)
{
    /* Don't create stream for ourselves */
    if (client_id == e->client_id)
        return NULL;

    /* Find existing */
    remote_stream_t *s = find_stream(e, client_id);
    if (s) return s;

    /* Find empty slot */
    for (int i = 0; i < AE_MAX_USERS_PER_CH; i++) {
        if (!e->streams[i].active) {
            s = &e->streams[i];
            memset(s, 0, sizeof(*s));
            s->client_id = client_id;
            s->active = 1;
            s->volume = 1.0f;
            s->volume_target = 1.0f;
            s->decoder = ae_opus_decoder_create();
            s->jitter = ae_jitter_create(
                e->config.jitter_buffer_frames > 0 ? e->config.jitter_buffer_frames : 3,
                2,
                AE_JITTER_BUF_FRAMES
            );
            ae_rtp_seq_init(&s->seq_tracker);
            e->stream_count++;
            atomic_store(&e->active_streams, e->stream_count);
            return s;
        }
    }
    return NULL; /* all slots full */
}

static void destroy_stream(ae_engine_t *e, remote_stream_t *s)
{
    if (!s || !s->active) return;
    if (s->decoder) ae_opus_decoder_destroy(s->decoder);
    if (s->jitter)  ae_jitter_destroy(s->jitter);
    s->active = 0;
    s->decoder = NULL;
    s->jitter = NULL;
    e->stream_count--;
    atomic_store(&e->active_streams, e->stream_count);
}

/* ═══════════════════════════════════════════════════════════════════ */
/* Public API                                                         */
/* ═══════════════════════════════════════════════════════════════════ */

ae_config_t ae_config_default(void)
{
    ae_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate          = AE_SAMPLE_RATE;
    cfg.channels             = AE_CHANNELS;
    cfg.frame_size           = AE_FRAME_SIZE;
    cfg.opus_bitrate         = AE_OPUS_BITRATE;
    cfg.opus_fec             = true;
    cfg.opus_complexity      = 10;
    cfg.jitter_buffer_frames = AE_JITTER_BUF_FRAMES;
    return cfg;
}

ae_engine_t *ae_engine_create(const ae_config_t *config)
{
    ae_engine_t *e = (ae_engine_t *)calloc(1, sizeof(ae_engine_t));
    if (!e)
        return NULL;

    /* Copy config or use defaults */
    if (config) {
        e->config = *config;
    } else {
        e->config = ae_config_default();
    }

    /* Apply defaults for zero fields */
    if (e->config.sample_rate == 0) e->config.sample_rate = AE_SAMPLE_RATE;
    if (e->config.channels == 0)    e->config.channels = AE_CHANNELS;
    if (e->config.frame_size == 0)  e->config.frame_size = AE_FRAME_SIZE;
    if (e->config.opus_bitrate == 0) e->config.opus_bitrate = AE_OPUS_BITRATE;
    if (e->config.opus_complexity == 0) e->config.opus_complexity = 10;
    if (e->config.jitter_buffer_frames == 0) e->config.jitter_buffer_frames = AE_JITTER_BUF_FRAMES;

    /* Create encoder */
    e->encoder = ae_opus_encoder_create(e->config.opus_bitrate,
                                        e->config.opus_complexity);
    if (!e->encoder) {
        free(e);
        return NULL;
    }

    /* Create ring buffers */
    e->capture_ring = ae_ringbuf_create(CAPTURE_RING_FRAMES, AE_CHANNELS);
    e->playback_ring = ae_ringbuf_create(PLAYBACK_RING_FRAMES, AE_CHANNELS);
    if (!e->capture_ring || !e->playback_ring) {
        ae_engine_destroy(e);
        return NULL;
    }

    /* Create mixer */
    e->mixer = ae_mixer_create(AE_MAX_USERS_PER_CH);
    if (!e->mixer) {
        ae_engine_destroy(e);
        return NULL;
    }

    /* Create crypto */
    e->crypto = ae_crypto_init();
    if (!e->crypto) {
        ae_engine_destroy(e);
        return NULL;
    }

    /* Create network (socket not opened yet) */
    e->network = ae_net_create();
    if (!e->network) {
        ae_engine_destroy(e);
        return NULL;
    }

    /* Initialize atomics */
    atomic_init(&e->connected, false);
    atomic_init(&e->transmitting, false);
    atomic_init(&e->packets_sent, 0);
    atomic_init(&e->packets_received, 0);
    atomic_init(&e->buffer_underruns, 0);
    atomic_init(&e->active_streams, 0);

    e->master_volume = 1.0f;
    e->tx_sequence = 0;

    return e;
}

void ae_engine_destroy(ae_engine_t *engine)
{
    if (!engine)
        return;

    /* Disconnect first */
    ae_engine_disconnect(engine);

    /* Destroy all remote streams */
    for (int i = 0; i < AE_MAX_USERS_PER_CH; i++) {
        if (engine->streams[i].active)
            destroy_stream(engine, &engine->streams[i]);
    }

    /* Destroy subsystems */
    if (engine->encoder)      ae_opus_encoder_destroy(engine->encoder);
    if (engine->capture_ring) ae_ringbuf_destroy(engine->capture_ring);
    if (engine->playback_ring) ae_ringbuf_destroy(engine->playback_ring);
    if (engine->mixer)        ae_mixer_destroy(engine->mixer);
    if (engine->crypto)       ae_crypto_cleanup(engine->crypto);
    if (engine->network)      ae_net_destroy(engine->network);

    free(engine);
}

/* ─── Callbacks ───────────────────────────────────────────────────── */

void ae_engine_set_capture_callback(ae_engine_t *engine,
                                    ae_capture_callback_t cb, void *user_data)
{
    if (!engine) return;
    engine->capture_cb = cb;
    engine->capture_ud = user_data;
}

void ae_engine_set_playback_callback(ae_engine_t *engine,
                                     ae_playback_callback_t cb, void *user_data)
{
    if (!engine) return;
    engine->playback_cb = cb;
    engine->playback_ud = user_data;
}

void ae_engine_set_event_callback(ae_engine_t *engine,
                                  ae_event_callback_t cb, void *user_data)
{
    if (!engine) return;
    engine->event_cb = cb;
    engine->event_ud = user_data;
}

/* ─── Connection ──────────────────────────────────────────────────── */

ae_error_t ae_engine_connect(ae_engine_t *engine, const char *server_host,
                             int udp_port, int ws_port, const char *auth_token)
{
    if (!engine || !server_host)
        return AE_ERR_INVALID_PARAM;

    (void)ws_port;    /* WebSocket connection handled separately in POC */
    (void)auth_token;

    /* Save connection info */
    strncpy(engine->server_host, server_host, sizeof(engine->server_host) - 1);
    engine->udp_port = udp_port;

    /* Connect UDP socket */
    if (ae_net_connect(engine->network, server_host, (uint16_t)udp_port) != 0)
        return AE_ERR_NETWORK;

    atomic_store(&engine->connected, true);
    engine->last_keepalive_ms = ae_net_time_ms();

    /* Fire event */
    if (engine->event_cb) {
        ae_event_t ev = {0};
        ev.type = AE_EVENT_CONNECTED;
        engine->event_cb(&ev, engine->event_ud);
    }

    return AE_OK;
}

void ae_engine_disconnect(ae_engine_t *engine)
{
    if (!engine) return;

    atomic_store(&engine->transmitting, false);
    atomic_store(&engine->connected, false);

    ae_net_disconnect(engine->network);

    /* Clear channel state */
    engine->channel.active = 0;

    /* Fire event */
    if (engine->event_cb) {
        ae_event_t ev = {0};
        ev.type = AE_EVENT_DISCONNECTED;
        engine->event_cb(&ev, engine->event_ud);
    }
}

bool ae_engine_is_connected(const ae_engine_t *engine)
{
    if (!engine) return false;
    return atomic_load(&engine->connected);
}

/* ─── Channel management ─────────────────────────────────────────── */

ae_error_t ae_engine_join_channel(ae_engine_t *engine, const char *channel_id)
{
    if (!engine || !channel_id)
        return AE_ERR_INVALID_PARAM;
    if (!atomic_load(&engine->connected))
        return AE_ERR_NOT_CONNECTED;

    /* Leave current channel first if in one */
    if (engine->channel.active) {
        ae_engine_leave_channel(engine, engine->channel.channel_id_str);
    }

    /* Set up channel state */
    strncpy(engine->channel.channel_id_str, channel_id,
            sizeof(engine->channel.channel_id_str) - 1);
    engine->channel.channel_id_num = channel_str_to_id(channel_id);
    engine->channel.active = 1;
    engine->channel.transmitting = 0;
    engine->channel.volume = 1.0f;

    /* Set up encryption key for this channel */
    ae_crypto_set_channel_key(engine->crypto,
                              engine->channel.channel_id_num, NULL);

    return AE_OK;
}

ae_error_t ae_engine_leave_channel(ae_engine_t *engine, const char *channel_id)
{
    if (!engine || !channel_id)
        return AE_ERR_INVALID_PARAM;

    /* Stop transmitting */
    atomic_store(&engine->transmitting, false);
    engine->channel.transmitting = 0;

    /* Destroy all remote streams for this channel */
    for (int i = 0; i < AE_MAX_USERS_PER_CH; i++) {
        if (engine->streams[i].active)
            destroy_stream(engine, &engine->streams[i]);
    }

    engine->channel.active = 0;

    return AE_OK;
}

void ae_engine_leave_all_channels(ae_engine_t *engine)
{
    if (!engine) return;
    if (engine->channel.active)
        ae_engine_leave_channel(engine, engine->channel.channel_id_str);
}

/* ─── Transmit control ────────────────────────────────────────────── */

ae_error_t ae_engine_start_transmit(ae_engine_t *engine, const char *channel_id)
{
    if (!engine || !channel_id)
        return AE_ERR_INVALID_PARAM;
    if (!engine->channel.active)
        return AE_ERR_NOT_CONNECTED;

    engine->channel.transmitting = 1;
    atomic_store(&engine->transmitting, true);

    return AE_OK;
}

ae_error_t ae_engine_stop_transmit(ae_engine_t *engine, const char *channel_id)
{
    if (!engine || !channel_id)
        return AE_ERR_INVALID_PARAM;

    engine->channel.transmitting = 0;
    atomic_store(&engine->transmitting, false);

    return AE_OK;
}

/* ─── Volume ──────────────────────────────────────────────────────── */

ae_error_t ae_engine_set_channel_volume(ae_engine_t *engine,
                                        const char *channel_id, float volume)
{
    if (!engine || !channel_id)
        return AE_ERR_INVALID_PARAM;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    engine->channel.volume = volume;
    return AE_OK;
}

void ae_engine_set_master_volume(ae_engine_t *engine, float volume)
{
    if (!engine) return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    engine->master_volume = volume;
    ae_mixer_set_master_volume(engine->mixer, volume);
}

ae_error_t ae_engine_set_channel_muted(ae_engine_t *engine,
                                       const char *channel_id, bool muted)
{
    if (!engine || !channel_id)
        return AE_ERR_INVALID_PARAM;
    /* Muting is just setting volume to 0 */
    engine->channel.volume = muted ? 0.0f : 1.0f;
    return AE_OK;
}

/* ─── Audio I/O (called from platform audio thread) ───────────────── */

ae_error_t ae_engine_write_capture(ae_engine_t *engine, const float *samples,
                                   int frame_count)
{
    if (!engine || !samples || frame_count <= 0)
        return AE_ERR_INVALID_PARAM;

    int written = ae_ringbuf_write(engine->capture_ring, samples, frame_count);
    return (written == frame_count) ? AE_OK : AE_ERR_FULL;
}

int ae_engine_read_playback(ae_engine_t *engine, float *samples, int frame_count)
{
    if (!engine || !samples || frame_count <= 0)
        return 0;

    int read = ae_ringbuf_read(engine->playback_ring, samples, frame_count);

    /* If we didn't get enough frames, zero-fill the rest */
    if (read < frame_count) {
        int remaining_samples = (frame_count - read) * AE_CHANNELS;
        memset(samples + read * AE_CHANNELS, 0,
               (size_t)remaining_samples * sizeof(float));
    }

    return read;
}

/* ─── Main processing tick ────────────────────────────────────────── */

/*
 * ae_engine_process() is called every 20ms from the audio thread.
 * It MUST NOT allocate memory or acquire mutexes.
 * All buffers are pre-allocated in the engine struct.
 */
void ae_engine_process(ae_engine_t *engine)
{
    if (!engine)
        return;

    bool is_connected = atomic_load(&engine->connected);

    /* ── Step 1: Capture → Encode → Encrypt → Send ─────────────────── */

    if (is_connected && atomic_load(&engine->transmitting)) {
        /* Read one frame from capture ring buffer */
        int cap_frames = ae_ringbuf_read(engine->capture_ring,
                                         engine->capture_frame, AE_FRAME_SIZE);

        if (cap_frames == AE_FRAME_SIZE) {
            /* Encode to Opus */
            int encoded_len = ae_opus_encode(engine->encoder,
                                             engine->capture_frame,
                                             engine->encode_buf,
                                             sizeof(engine->encode_buf));

            if (encoded_len > 0) {
                /* Build RTP header for the routing part */
                ae_rtp_header_t hdr = {0};
                hdr.server_id   = engine->server_id;
                hdr.channel_id  = engine->channel.channel_id_num;
                hdr.client_id   = engine->client_id;
                hdr.packet_type = AE_RTP_TYPE_AUDIO;
                hdr.flags       = AE_RTP_FLAG_STEREO;

                /* Serialize the routing header for use as AAD */
                uint8_t routing_hdr[AE_RTP_HEADER_SIZE];
                hdr.payload_length = 0; /* will be set by build_packet */
                ae_rtp_build_packet(&hdr, NULL, 0, routing_hdr,
                                    AE_RTP_HEADER_SIZE);

                /* Encrypt the Opus payload */
                int encrypted_len = ae_crypto_encrypt(
                    engine->crypto,
                    engine->channel.channel_id_num,
                    engine->client_id,
                    engine->tx_sequence,
                    routing_hdr, AE_RTP_HEADER_SIZE,
                    engine->encode_buf, encoded_len,
                    engine->encrypt_buf, sizeof(engine->encrypt_buf)
                );

                if (encrypted_len > 0) {
                    /* Build final packet: header + encrypted payload */
                    int packet_len = ae_rtp_build_packet(
                        &hdr,
                        engine->encrypt_buf, encrypted_len,
                        engine->packet_buf, sizeof(engine->packet_buf)
                    );

                    if (packet_len > 0) {
                        ae_net_send(engine->network,
                                    engine->packet_buf, packet_len);
                        engine->tx_sequence++;
                        atomic_fetch_add(&engine->packets_sent, 1);
                    }
                }
            }
        }
    }

    /* ── Step 2: Receive → Decrypt → Decode → Jitter buffer ────────── */

    if (is_connected) {
        for (int pkt = 0; pkt < MAX_RECV_PER_TICK; pkt++) {
            int recv_len = ae_net_recv(engine->network,
                                       engine->recv_buf,
                                       sizeof(engine->recv_buf));
            if (recv_len <= 0)
                break;

            atomic_fetch_add(&engine->packets_received, 1);

            /* Parse routing header */
            ae_rtp_header_t hdr;
            if (ae_rtp_parse_header(engine->recv_buf, recv_len, &hdr) != 0)
                continue;

            /* Skip our own packets */
            if (hdr.client_id == engine->client_id)
                continue;

            /* Handle packet types */
            switch (hdr.packet_type) {
            case AE_RTP_TYPE_AUDIO: {
                if (hdr.payload_length == 0 || !engine->channel.active)
                    break;

                /* Get or create remote stream */
                remote_stream_t *stream = find_stream(engine, hdr.client_id);
                if (!stream) {
                    stream = create_stream(engine, hdr.client_id);
                    if (!stream) break;
                }

                /* Get encrypted payload */
                const uint8_t *payload = ae_rtp_get_payload(engine->recv_buf,
                                                            recv_len);
                if (!payload) break;

                /* Decrypt */
                uint8_t decrypted[AE_MAX_PACKET_SIZE];
                int decrypted_len = ae_crypto_decrypt(
                    engine->crypto,
                    hdr.channel_id,
                    engine->recv_buf, AE_RTP_HEADER_SIZE,  /* AAD */
                    payload, hdr.payload_length,
                    decrypted, sizeof(decrypted)
                );

                if (decrypted_len <= 0)
                    break;

                /* Update sequence tracker */
                /* Extract sequence from nonce (it's in the IV of the payload) */
                ae_rtp_seq_update(&stream->seq_tracker, stream->seq_tracker.max_seq + 1);

                /* Decode Opus to PCM */
                int decoded = ae_opus_decode(stream->decoder,
                                             decrypted, decrypted_len,
                                             engine->decode_frame);

                if (decoded > 0) {
                    /* Push decoded frame into jitter buffer */
                    ae_jitter_push(stream->jitter, engine->decode_frame,
                                   stream->seq_tracker.max_seq);
                }
                break;
            }

            case AE_RTP_TYPE_KEEPALIVE:
                /* Server keepalive -- nothing to do */
                break;

            case AE_RTP_TYPE_PONG:
                /* RTT measurement: could extract timestamp from payload */
                break;

            default:
                break;
            }
        }

        /* ── Step 3: Send keepalive if needed ──────────────────────── */

        uint64_t now = ae_net_time_ms();
        if (now - engine->last_keepalive_ms >= KEEPALIVE_INTERVAL_MS) {
            uint8_t ka_buf[AE_RTP_HEADER_SIZE];
            int ka_len = ae_rtp_build_keepalive(
                engine->server_id,
                engine->channel.active ? engine->channel.channel_id_num : 0,
                engine->client_id,
                ka_buf, sizeof(ka_buf)
            );
            if (ka_len > 0)
                ae_net_send(engine->network, ka_buf, ka_len);

            engine->last_keepalive_ms = now;
        }
    }

    /* ── Step 4: Pop from jitter buffers → Mix → Playback ring ─────── */

    int mix_count = 0;

    for (int i = 0; i < AE_MAX_USERS_PER_CH; i++) {
        if (!engine->streams[i].active || !engine->streams[i].jitter)
            continue;

        remote_stream_t *s = &engine->streams[i];

        /* Try to pop a frame from the jitter buffer */
        int pop_result = ae_jitter_pop(s->jitter, engine->stream_frames[mix_count]);

        if (pop_result < 0) {
            /* Jitter buffer empty -- try PLC */
            if (s->decoder) {
                int plc_result = ae_opus_decode_plc(s->decoder,
                                                     engine->stream_frames[mix_count]);
                if (plc_result <= 0) {
                    /* PLC failed -- use silence */
                    memset(engine->stream_frames[mix_count], 0,
                           AE_FRAME_SIZE * AE_CHANNELS * sizeof(float));
                }
            } else {
                memset(engine->stream_frames[mix_count], 0,
                       AE_FRAME_SIZE * AE_CHANNELS * sizeof(float));
            }
            atomic_fetch_add(&engine->buffer_underruns, 1);
        }

        engine->mix_inputs[mix_count] = engine->stream_frames[mix_count];
        engine->mix_volumes[mix_count] = s->volume * engine->channel.volume;
        mix_count++;
    }

    if (mix_count > 0) {
        /* Mix all streams */
        ae_mixer_mix(engine->mixer,
                     (const float *const *)engine->mix_inputs,
                     engine->mix_volumes,
                     mix_count,
                     engine->mix_output);

        /* Write mixed output to playback ring buffer */
        ae_ringbuf_write(engine->playback_ring,
                         engine->mix_output, AE_FRAME_SIZE);
    } else if (is_connected && engine->channel.active) {
        /* No streams but in channel -- write silence to keep the buffer flowing */
        memset(engine->mix_output, 0,
               AE_FRAME_SIZE * AE_CHANNELS * sizeof(float));
        ae_ringbuf_write(engine->playback_ring,
                         engine->mix_output, AE_FRAME_SIZE);
    }
}

/* ─── Statistics ──────────────────────────────────────────────────── */

ae_stats_t ae_engine_get_stats(const ae_engine_t *engine)
{
    ae_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    if (!engine)
        return stats;

    stats.packets_sent     = atomic_load(&engine->packets_sent);
    stats.packets_received = atomic_load(&engine->packets_received);
    stats.buffer_underruns = atomic_load(&engine->buffer_underruns);
    stats.active_streams   = atomic_load(&engine->active_streams);
    stats.rtt_ms           = engine->rtt_ms;
    stats.joined_channels  = engine->channel.active ? 1 : 0;

    /* Compute packet loss from all streams */
    uint32_t total_lost = 0;
    uint32_t total_received = 0;
    for (int i = 0; i < AE_MAX_USERS_PER_CH; i++) {
        if (engine->streams[i].active) {
            total_lost     += engine->streams[i].seq_tracker.lost;
            total_received += engine->streams[i].seq_tracker.received;
        }
    }
    if (total_received > 0) {
        stats.packet_loss_pct = (float)total_lost / (float)(total_received + total_lost) * 100.0f;
    }

    return stats;
}
