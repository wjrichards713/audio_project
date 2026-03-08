/**
 * oboe-audio-io.h - Android audio I/O using Google Oboe
 *
 * This handles the platform-specific audio capture and playback on Android.
 * It uses Oboe which wraps AAudio (API 27+) for lowest latency.
 *
 * CRITICAL for fixing the Android popping issue:
 * - Uses float32 format (no int16 overflow during mixing)
 * - Uses 48kHz sample rate (matches Opus, avoids Android resampler)
 * - Uses Exclusive sharing mode (bypasses Android mixer)
 * - Uses callback-based I/O (no blocking, predictable timing)
 * - Sets buffer to 2x burst size for glitch-free playback
 */

#ifndef OBOE_AUDIO_IO_H
#define OBOE_AUDIO_IO_H

#include <oboe/Oboe.h>
#include <atomic>

extern "C" {
#include "audio_engine.h"
}

class OboeAudioIO : public oboe::AudioStreamDataCallback,
                    public oboe::AudioStreamErrorCallback {
public:
    OboeAudioIO(ae_engine_t *engine);
    ~OboeAudioIO();

    int start();
    void stop();

    // Oboe callbacks
    oboe::DataCallbackResult onAudioReady(
        oboe::AudioStream *stream,
        void *audioData,
        int32_t numFrames) override;

    void onErrorBeforeClose(oboe::AudioStream *stream, oboe::Result error) override;
    void onErrorAfterClose(oboe::AudioStream *stream, oboe::Result error) override;

private:
    ae_engine_t *engine_;
    oboe::ManagedStream outputStream_;
    oboe::ManagedStream inputStream_;
    std::atomic<bool> running_{false};

    // Pre-allocated buffers (no allocation on audio thread)
    float captureBuffer_[AE_FRAME_SIZE * AE_CHANNELS];
    float playbackBuffer_[AE_FRAME_SIZE * AE_CHANNELS];

    int openOutputStream();
    int openInputStream();
    void restartStream(oboe::AudioStream *stream);
};

#endif // OBOE_AUDIO_IO_H
