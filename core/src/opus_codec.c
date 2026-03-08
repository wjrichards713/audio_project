/**
 * @file opus_codec.c
 * @brief Opus encoder/decoder wrapper implementation.
 *
 * Uses OPUS_APPLICATION_AUDIO for high-quality music/audio streaming.
 */

#include "opus_codec.h"
#include "../include/audio_engine.h"

#include <opus/opus.h>
#include <stdlib.h>
#include <string.h>

/* ─── Internal structures ─────────────────────────────────────────── */

struct ae_opus_encoder {
    OpusEncoder *opus;
};

struct ae_opus_decoder {
    OpusDecoder *opus;
};

/* ─── Encoder ─────────────────────────────────────────────────────── */

ae_opus_encoder_t *ae_opus_encoder_create(int bitrate, int complexity)
{
    if (bitrate <= 0)
        bitrate = AE_OPUS_BITRATE;
    if (complexity <= 0 || complexity > 10)
        complexity = 10;

    ae_opus_encoder_t *enc = (ae_opus_encoder_t *)calloc(1, sizeof(*enc));
    if (!enc)
        return NULL;

    int error = 0;
    enc->opus = opus_encoder_create(AE_SAMPLE_RATE, AE_CHANNELS,
                                    OPUS_APPLICATION_AUDIO, &error);
    if (error != OPUS_OK || !enc->opus) {
        free(enc);
        return NULL;
    }

    /* Configure for high-quality music streaming */
    opus_encoder_ctl(enc->opus, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(enc->opus, OPUS_SET_COMPLEXITY(complexity));
    opus_encoder_ctl(enc->opus, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(enc->opus, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
    opus_encoder_ctl(enc->opus, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc->opus, OPUS_SET_PACKET_LOSS_PERC(5));
    opus_encoder_ctl(enc->opus, OPUS_SET_DTX(0));  /* keep continuous for music */

    return enc;
}

void ae_opus_encoder_destroy(ae_opus_encoder_t *enc)
{
    if (!enc)
        return;
    if (enc->opus)
        opus_encoder_destroy(enc->opus);
    free(enc);
}

int ae_opus_encode(ae_opus_encoder_t *enc, const float *pcm,
                   uint8_t *out, int out_size)
{
    if (!enc || !enc->opus || !pcm || !out)
        return -1;

    int ret = opus_encode_float(enc->opus, pcm, AE_FRAME_SIZE, out, out_size);
    return ret;  /* positive = bytes written, negative = opus error code */
}

/* ─── Decoder ─────────────────────────────────────────────────────── */

ae_opus_decoder_t *ae_opus_decoder_create(void)
{
    ae_opus_decoder_t *dec = (ae_opus_decoder_t *)calloc(1, sizeof(*dec));
    if (!dec)
        return NULL;

    int error = 0;
    dec->opus = opus_decoder_create(AE_SAMPLE_RATE, AE_CHANNELS, &error);
    if (error != OPUS_OK || !dec->opus) {
        free(dec);
        return NULL;
    }

    return dec;
}

void ae_opus_decoder_destroy(ae_opus_decoder_t *dec)
{
    if (!dec)
        return;
    if (dec->opus)
        opus_decoder_destroy(dec->opus);
    free(dec);
}

int ae_opus_decode(ae_opus_decoder_t *dec, const uint8_t *data, int data_len,
                   float *pcm)
{
    if (!dec || !dec->opus || !data || !pcm || data_len <= 0)
        return -1;

    int ret = opus_decode_float(dec->opus, data, data_len, pcm,
                                AE_FRAME_SIZE, /* decode_fec */ 0);
    return ret;  /* positive = samples per channel, negative = error */
}

int ae_opus_decode_plc(ae_opus_decoder_t *dec, float *pcm)
{
    if (!dec || !dec->opus || !pcm)
        return -1;

    /* Pass NULL data to trigger PLC */
    int ret = opus_decode_float(dec->opus, NULL, 0, pcm,
                                AE_FRAME_SIZE, /* decode_fec */ 0);
    return ret;
}
