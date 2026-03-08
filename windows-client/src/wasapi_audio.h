/**
 * wasapi_audio.h - Windows audio I/O using WASAPI
 *
 * WASAPI (Windows Audio Session API) provides the lowest-latency
 * audio on Windows. We use shared mode for compatibility.
 */

#ifndef WASAPI_AUDIO_H
#define WASAPI_AUDIO_H

#ifdef _WIN32

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <atomic>
#include <thread>

extern "C" {
#include "audio_engine.h"
}

class WasapiAudio {
public:
    WasapiAudio(ae_engine_t *engine);
    ~WasapiAudio();

    int start();
    void stop();

private:
    ae_engine_t *engine_;
    std::atomic<bool> running_{false};
    std::thread audioThread_;

    // WASAPI objects
    IMMDeviceEnumerator *enumerator_ = nullptr;
    IMMDevice *renderDevice_ = nullptr;
    IMMDevice *captureDevice_ = nullptr;
    IAudioClient *renderClient_ = nullptr;
    IAudioClient *captureClient_ = nullptr;
    IAudioRenderClient *renderService_ = nullptr;
    IAudioCaptureClient *captureService_ = nullptr;

    UINT32 renderBufferSize_ = 0;
    UINT32 captureBufferSize_ = 0;

    int initCOM();
    int openRenderDevice();
    int openCaptureDevice();
    void audioLoop();
    void cleanup();
};

#endif // _WIN32
#endif // WASAPI_AUDIO_H
