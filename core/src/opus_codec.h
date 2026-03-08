/**
 * @file opus_codec.h
 * @brief Opus encoder/decoder wrapper.
 *
 * Configured for high-quality music streaming:
 *   - OPUS_APPLICATION_AUDIO (NOT VOIP)
 *   - 48 kHz stereo
 *   - 128 kbps
 *   - Complexity 10
 *   - OPUS_SIGNAL_MUSIC
 *   - Fullband
 *   - FEC enabled
 */

#ifndef OPUS_CODEC_H
#define OPUS_CODEC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum encoded packet size in bytes */
#define AE_OPUS_MAX_PACKET  4000

/* Forward-declare opaque Opus types so we don't need opus.h in this header */
typedef struct ae_opus_encoder ae_opus_encoder_t;
typedef struct ae_opus_decoder ae_opus_decoder_t;

/**
 * Create an Opus encoder for stereo audio at 48 kHz.
 * @param bitrate     Target bitrate in bps (0 = default 128000).
 * @param complexity  Encoder complexity 0-10 (0 = default 10).
 * @return Encoder handle, or NULL on failure.
 */
ae_opus_encoder_t *ae_opus_encoder_create(int bitrate, int complexity);

/** Destroy an Opus encoder. */
void ae_opus_encoder_destroy(ae_opus_encoder_t *enc);

/**
 * Encode one frame of interleaved float32 PCM to Opus.
 * @param enc         Encoder handle.
 * @param pcm         Input: AE_FRAME_SIZE * AE_CHANNELS float32 samples.
 * @param out         Output buffer (must be >= AE_OPUS_MAX_PACKET bytes).
 * @param out_size    Size of output buffer in bytes.
 * @return Number of bytes written to out, or negative on error.
 */
int ae_opus_encode(ae_opus_encoder_t *enc, const float *pcm,
                   uint8_t *out, int out_size);

/**
 * Create an Opus decoder for stereo audio at 48 kHz.
 * @return Decoder handle, or NULL on failure.
 */
ae_opus_decoder_t *ae_opus_decoder_create(void);

/** Destroy an Opus decoder. */
void ae_opus_decoder_destroy(ae_opus_decoder_t *dec);

/**
 * Decode an Opus packet to interleaved float32 PCM.
 * @param dec         Decoder handle.
 * @param data        Opus packet data.
 * @param data_len    Opus packet length in bytes.
 * @param pcm         Output: AE_FRAME_SIZE * AE_CHANNELS float32 samples.
 * @return Number of samples per channel decoded, or negative on error.
 */
int ae_opus_decode(ae_opus_decoder_t *dec, const uint8_t *data, int data_len,
                   float *pcm);

/**
 * Packet Loss Concealment: generate a replacement frame when a packet is lost.
 * @param dec         Decoder handle.
 * @param pcm         Output: AE_FRAME_SIZE * AE_CHANNELS float32 samples.
 * @return Number of samples per channel generated, or negative on error.
 */
int ae_opus_decode_plc(ae_opus_decoder_t *dec, float *pcm);

#ifdef __cplusplus
}
#endif

#endif /* OPUS_CODEC_H */
