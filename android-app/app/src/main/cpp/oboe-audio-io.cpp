/**
 * oboe-audio-io.cpp - Android audio I/O using Google Oboe
 *
 * This is the platform-specific audio layer for Android.
 * It captures microphone audio and plays mixed audio using Oboe/AAudio.
 *
 * KEY DESIGN DECISIONS (fixing the Android popping issue):
 *
 * 1. FLOAT32 format — prevents integer overflow when mixing multiple streams
 * 2. 48000 Hz sample rate — matches Opus native rate, avoids AudioFlinger resampler
 *    (the resampler's 44.1→48kHz ratio of 147:160 creates ~4Hz clicks)
 * 3. Exclusive sharing mode — bypasses Android's audio mixer for lowest latency
 * 4. Callback-driven — Oboe calls us at precise intervals, no blocking writes
 * 5. Buffer = 2x burst — gives enough headroom to avoid underruns
 * 6. NO memory allocation in callbacks — everything pre-allocated
 * 7. NO mutexes in callbacks — engine uses lock-free data structures
 */

#include "oboe-audio-io.h"
#include <android/log.h>
#include <cstring>

#define LOG_TAG "OboeAudioIO"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

OboeAudioIO::OboeAudioIO(ae_engine_t *engine)
    : engine_(engine) {
    // Zero out pre-allocated buffers
    memset(captureBuffer_, 0, sizeof(captureBuffer_));
    memset(playbackBuffer_, 0, sizeof(playbackBuffer_));
}

OboeAudioIO::~OboeAudioIO() {
    stop();
}

int OboeAudioIO::openOutputStream() {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
           ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
           ->setSharingMode(oboe::SharingMode::Exclusive)  // Bypass Android mixer
           ->setFormat(oboe::AudioFormat::Float)            // CRITICAL: float32
           ->setChannelCount(AE_CHANNELS)                   // Stereo
           ->setSampleRate(AE_SAMPLE_RATE)                  // 48kHz — matches Opus
           ->setFramesPerCallback(AE_FRAME_SIZE)            // 960 = 20ms
           ->setDataCallback(this)
           ->setErrorCallback(this)
           ->setUsage(oboe::Usage::VoiceCommunication)
           ->setContentType(oboe::ContentType::Speech);

    oboe::Result result = builder.openManagedStream(outputStream_);
    if (result != oboe::Result::OK) {
        LOGE("Failed to open output stream: %s", oboe::convertToText(result));
        return -1;
    }

    // Set buffer to 2x burst size for glitch-free playback
    auto framesPerBurst = outputStream_->getFramesPerBurst();
    outputStream_->setBufferSizeInFrames(framesPerBurst * 2);

    LOGI("Output stream opened: rate=%d, channels=%d, format=%d, burst=%d, buffer=%d",
         outputStream_->getSampleRate(),
         outputStream_->getChannelCount(),
         (int)outputStream_->getFormat(),
         framesPerBurst,
         outputStream_->getBufferSizeInFrames());

    // Verify we got what we asked for
    if (outputStream_->getSampleRate() != AE_SAMPLE_RATE) {
        LOGE("WARNING: Got sample rate %d instead of %d — resampling will occur!",
             outputStream_->getSampleRate(), AE_SAMPLE_RATE);
    }

    return 0;
}

int OboeAudioIO::openInputStream() {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input)
           ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
           ->setSharingMode(oboe::SharingMode::Exclusive)
           ->setFormat(oboe::AudioFormat::Float)
           ->setChannelCount(AE_CHANNELS)
           ->setSampleRate(AE_SAMPLE_RATE)
           ->setFramesPerCallback(AE_FRAME_SIZE)
           ->setInputPreset(oboe::InputPreset::VoiceCommunication);

    oboe::Result result = builder.openManagedStream(inputStream_);
    if (result != oboe::Result::OK) {
        LOGE("Failed to open input stream: %s", oboe::convertToText(result));
        return -1;
    }

    LOGI("Input stream opened: rate=%d, channels=%d",
         inputStream_->getSampleRate(),
         inputStream_->getChannelCount());

    return 0;
}

int OboeAudioIO::start() {
    if (running_.load()) return 0;

    int err = openOutputStream();
    if (err != 0) return err;

    err = openInputStream();
    if (err != 0) return err;

    // Start output first, then input
    oboe::Result result = outputStream_->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start output: %s", oboe::convertToText(result));
        return -1;
    }

    result = inputStream_->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start input: %s", oboe::convertToText(result));
        return -1;
    }

    running_.store(true);
    LOGI("Audio I/O started");
    return 0;
}

void OboeAudioIO::stop() {
    if (!running_.load()) return;
    running_.store(false);

    if (inputStream_) {
        inputStream_->requestStop();
        inputStream_->close();
    }
    if (outputStream_) {
        outputStream_->requestStop();
        outputStream_->close();
    }

    LOGI("Audio I/O stopped");
}

/**
 * CRITICAL CALLBACK - Called by Oboe on the audio thread every 20ms.
 *
 * Rules for this function:
 * - NO memory allocation (malloc, new, etc.)
 * - NO mutex locking
 * - NO system calls (logging, file I/O)
 * - NO JNI calls
 * - MUST complete within the frame duration (~20ms)
 */
oboe::DataCallbackResult OboeAudioIO::onAudioReady(
    oboe::AudioStream *stream,
    void *audioData,
    int32_t numFrames) {

    if (!running_.load()) {
        return oboe::DataCallbackResult::Stop;
    }

    auto *outputBuffer = static_cast<float *>(audioData);

    if (stream == outputStream_.get()) {
        // ─── PLAYBACK PATH ───
        // Read captured audio from input stream (non-blocking)
        if (inputStream_ && inputStream_->getState() == oboe::StreamState::Started) {
            auto readResult = inputStream_->read(
                captureBuffer_,
                numFrames,
                0  // non-blocking (0 timeout)
            );
            if (readResult.value() > 0) {
                // Feed captured audio into the engine
                ae_engine_write_capture(engine_, captureBuffer_, readResult.value());
            }
        }

        // Process one frame: encodes captured audio, receives network packets,
        // decodes incoming streams, mixes everything
        ae_engine_process(engine_);

        // Read mixed output from the engine
        int framesRead = ae_engine_read_playback(engine_, outputBuffer, numFrames);

        // If engine didn't provide enough frames, zero-fill the rest
        if (framesRead < numFrames) {
            memset(outputBuffer + (framesRead * AE_CHANNELS), 0,
                   (numFrames - framesRead) * AE_CHANNELS * sizeof(float));
        }
    }

    return oboe::DataCallbackResult::Continue;
}

void OboeAudioIO::onErrorBeforeClose(oboe::AudioStream *stream, oboe::Result error) {
    LOGE("Audio stream error before close: %s", oboe::convertToText(error));
}

void OboeAudioIO::onErrorAfterClose(oboe::AudioStream *stream, oboe::Result error) {
    LOGE("Audio stream error after close: %s — restarting", oboe::convertToText(error));
    // Oboe recommends restarting on error
    if (running_.load()) {
        restartStream(stream);
    }
}

void OboeAudioIO::restartStream(oboe::AudioStream *stream) {
    if (stream == outputStream_.get()) {
        outputStream_->close();
        openOutputStream();
        outputStream_->requestStart();
    } else if (stream == inputStream_.get()) {
        inputStream_->close();
        openInputStream();
        inputStream_->requestStart();
    }
}
