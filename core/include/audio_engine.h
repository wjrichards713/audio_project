/**
 * audio_engine.h - Cross-platform real-time audio engine
 *
 * This is the public API for the shared audio engine library.
 * All platforms (Android, iOS, macOS, Windows, RPi) use this same API.
 */

#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
  #ifdef AE_BUILDING_DLL
    #define AE_API __declspec(dllexport)
  #else
    #define AE_API __declspec(dllimport)
  #endif
#else
  #define AE_API __attribute__((visibility("default")))
#endif

/* ─── Constants ────────────────────────────────────────────────────── */

#define AE_SAMPLE_RATE       48000
#define AE_CHANNELS          2
#define AE_FRAME_SIZE        960     /* 20ms at 48kHz */
#define AE_FRAME_DURATION_MS 20
#define AE_MAX_CHANNELS      16
#define AE_MAX_USERS_PER_CH  64
#define AE_MAX_PACKET_SIZE   1400
#define AE_JITTER_BUF_FRAMES 10
#define AE_OPUS_BITRATE      128000

/* ─── Error Codes ──────────────────────────────────────────────────── */

typedef enum {
    AE_OK = 0,
    AE_ERR_INVALID_PARAM = -1,
    AE_ERR_NO_MEMORY = -2,
    AE_ERR_OPUS_INIT = -3,
    AE_ERR_NETWORK = -4,
    AE_ERR_CRYPTO = -5,
    AE_ERR_FULL = -6,
    AE_ERR_NOT_FOUND = -7,
    AE_ERR_NOT_CONNECTED = -8,
} ae_error_t;

/* ─── Opaque Types ─────────────────────────────────────────────────── */

typedef struct ae_engine ae_engine_t;

/* ─── Callback Types ───────────────────────────────────────────────── */

typedef int (*ae_capture_callback_t)(float *buffer, int frames, void *user_data);
typedef void (*ae_playback_callback_t)(const float *buffer, int frames, void *user_data);

typedef enum {
    AE_EVENT_CONNECTED,
    AE_EVENT_DISCONNECTED,
    AE_EVENT_USER_JOINED,
    AE_EVENT_USER_LEFT,
    AE_EVENT_USER_SPEAKING,
    AE_EVENT_USER_STOPPED,
    AE_EVENT_CHANNEL_KEY_UPDATE,
    AE_EVENT_SERVER_MIGRATE,
    AE_EVENT_ERROR,
} ae_event_type_t;

typedef struct {
    ae_event_type_t type;
    const char *channel_id;
    const char *client_id;
    const char *user_name;
    const char *message;
    const char *new_server;
} ae_event_t;

typedef void (*ae_event_callback_t)(const ae_event_t *event, void *user_data);

/* ─── Engine Configuration ─────────────────────────────────────────── */

typedef struct {
    int sample_rate;
    int channels;
    int frame_size;
    int opus_bitrate;
    bool opus_fec;
    int opus_complexity;
    int jitter_buffer_frames;
} ae_config_t;

AE_API ae_config_t ae_config_default(void);

/* ─── Engine Lifecycle ─────────────────────────────────────────────── */

AE_API ae_engine_t *ae_engine_create(const ae_config_t *config);
AE_API void ae_engine_destroy(ae_engine_t *engine);

/* ─── Audio I/O Callbacks ──────────────────────────────────────────── */

AE_API void ae_engine_set_capture_callback(ae_engine_t *engine, ae_capture_callback_t cb, void *user_data);
AE_API void ae_engine_set_playback_callback(ae_engine_t *engine, ae_playback_callback_t cb, void *user_data);
AE_API void ae_engine_set_event_callback(ae_engine_t *engine, ae_event_callback_t cb, void *user_data);

/* ─── Connection Management ────────────────────────────────────────── */

AE_API ae_error_t ae_engine_connect(ae_engine_t *engine, const char *server_host, int udp_port, int ws_port, const char *auth_token);
AE_API void ae_engine_disconnect(ae_engine_t *engine);
AE_API bool ae_engine_is_connected(const ae_engine_t *engine);

/* ─── Channel Management ──────────────────────────────────────────── */

AE_API ae_error_t ae_engine_join_channel(ae_engine_t *engine, const char *channel_id);
AE_API ae_error_t ae_engine_leave_channel(ae_engine_t *engine, const char *channel_id);
AE_API void ae_engine_leave_all_channels(ae_engine_t *engine);

/* ─── Transmission Control ─────────────────────────────────────────── */

AE_API ae_error_t ae_engine_start_transmit(ae_engine_t *engine, const char *channel_id);
AE_API ae_error_t ae_engine_stop_transmit(ae_engine_t *engine, const char *channel_id);

/* ─── Volume Control ───────────────────────────────────────────────── */

AE_API ae_error_t ae_engine_set_channel_volume(ae_engine_t *engine, const char *channel_id, float volume);
AE_API void ae_engine_set_master_volume(ae_engine_t *engine, float volume);
AE_API ae_error_t ae_engine_set_channel_muted(ae_engine_t *engine, const char *channel_id, bool muted);

/* ─── Audio Processing ─────────────────────────────────────────────── */

AE_API void ae_engine_process(ae_engine_t *engine);
AE_API ae_error_t ae_engine_write_capture(ae_engine_t *engine, const float *samples, int frame_count);
AE_API int ae_engine_read_playback(ae_engine_t *engine, float *samples, int frame_count);

/* ─── Statistics ───────────────────────────────────────────────────── */

typedef struct {
    float rtt_ms;
    float jitter_ms;
    float packet_loss_pct;
    int buffer_underruns;
    int active_streams;
    int joined_channels;
    uint64_t packets_sent;
    uint64_t packets_received;
} ae_stats_t;

AE_API ae_stats_t ae_engine_get_stats(const ae_engine_t *engine);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_ENGINE_H */
