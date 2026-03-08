/**
 * wasapi_audio.cpp - Windows WASAPI audio I/O
 *
 * Uses WASAPI in shared mode with event-driven callbacks.
 * 48kHz float32 stereo to match the engine's format.
 */

#ifdef _WIN32

#include "wasapi_audio.h"
#include <functiondiscoverykeys_devpkey.h>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

// WASAPI reference time units (100-nanosecond intervals)
#define REFTIMES_PER_SEC  10000000
#define REFTIMES_PER_MILLISEC 10000

WasapiAudio::WasapiAudio(ae_engine_t *engine) : engine_(engine) {}

WasapiAudio::~WasapiAudio() {
    stop();
}

int WasapiAudio::initCOM() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "COM init failed: 0x%lx\n", hr);
        return -1;
    }

    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        (void **)&enumerator_
    );
    if (FAILED(hr)) {
        fprintf(stderr, "Failed to create device enumerator: 0x%lx\n", hr);
        return -1;
    }
    return 0;
}

int WasapiAudio::openRenderDevice() {
    HRESULT hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &renderDevice_);
    if (FAILED(hr)) {
        fprintf(stderr, "No render device found\n");
        return -1;
    }

    hr = renderDevice_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&renderClient_);
    if (FAILED(hr)) return -1;

    // Configure format: 48kHz, stereo, float32
    WAVEFORMATEXTENSIBLE wfx = {};
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = AE_CHANNELS;
    wfx.Format.nSamplesPerSec = AE_SAMPLE_RATE;
    wfx.Format.wBitsPerSample = 32;
    wfx.Format.nBlockAlign = wfx.Format.nChannels * wfx.Format.wBitsPerSample / 8;
    wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
    wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    // 20ms buffer (matches our frame size)
    REFERENCE_TIME duration = AE_FRAME_DURATION_MS * REFTIMES_PER_MILLISEC * 2;

    hr = renderClient_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
        duration, 0,
        (WAVEFORMATEX *)&wfx, nullptr
    );
    if (FAILED(hr)) {
        fprintf(stderr, "Failed to init render client: 0x%lx\n", hr);
        return -1;
    }

    hr = renderClient_->GetBufferSize(&renderBufferSize_);
    if (FAILED(hr)) return -1;

    hr = renderClient_->GetService(__uuidof(IAudioRenderClient), (void **)&renderService_);
    if (FAILED(hr)) return -1;

    printf("Render device opened: buffer=%u frames\n", renderBufferSize_);
    return 0;
}

int WasapiAudio::openCaptureDevice() {
    HRESULT hr = enumerator_->GetDefaultAudioEndpoint(eCapture, eConsole, &captureDevice_);
    if (FAILED(hr)) {
        fprintf(stderr, "No capture device found\n");
        return -1;
    }

    hr = captureDevice_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&captureClient_);
    if (FAILED(hr)) return -1;

    WAVEFORMATEXTENSIBLE wfx = {};
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = AE_CHANNELS;
    wfx.Format.nSamplesPerSec = AE_SAMPLE_RATE;
    wfx.Format.wBitsPerSample = 32;
    wfx.Format.nBlockAlign = wfx.Format.nChannels * wfx.Format.wBitsPerSample / 8;
    wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
    wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    REFERENCE_TIME duration = AE_FRAME_DURATION_MS * REFTIMES_PER_MILLISEC * 2;

    hr = captureClient_->Initialize(
        AUDCLNT_SHAREMODE_SHARED, 0,
        duration, 0,
        (WAVEFORMATEX *)&wfx, nullptr
    );
    if (FAILED(hr)) {
        fprintf(stderr, "Failed to init capture client: 0x%lx\n", hr);
        return -1;
    }

    hr = captureClient_->GetBufferSize(&captureBufferSize_);
    if (FAILED(hr)) return -1;

    hr = captureClient_->GetService(__uuidof(IAudioCaptureClient), (void **)&captureService_);
    if (FAILED(hr)) return -1;

    printf("Capture device opened: buffer=%u frames\n", captureBufferSize_);
    return 0;
}

int WasapiAudio::start() {
    if (running_.load()) return 0;

    if (initCOM() != 0) return -1;
    if (openRenderDevice() != 0) return -1;
    if (openCaptureDevice() != 0) return -1;

    renderClient_->Start();
    captureClient_->Start();
    running_.store(true);

    audioThread_ = std::thread(&WasapiAudio::audioLoop, this);
    printf("WASAPI audio started\n");
    return 0;
}

void WasapiAudio::stop() {
    running_.store(false);
    if (audioThread_.joinable()) {
        audioThread_.join();
    }
    cleanup();
}

void WasapiAudio::audioLoop() {
    float captureBuffer[AE_FRAME_SIZE * AE_CHANNELS];
    float playbackBuffer[AE_FRAME_SIZE * AE_CHANNELS];

    while (running_.load()) {
        // ─── Capture ───
        UINT32 packetLength = 0;
        captureService_->GetNextPacketSize(&packetLength);
        while (packetLength > 0) {
            BYTE *data;
            UINT32 framesAvailable;
            DWORD flags;
            HRESULT hr = captureService_->GetBuffer(&data, &framesAvailable, &flags, nullptr, nullptr);
            if (SUCCEEDED(hr)) {
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    memset(captureBuffer, 0, framesAvailable * AE_CHANNELS * sizeof(float));
                } else {
                    size_t bytes = framesAvailable * AE_CHANNELS * sizeof(float);
                    if (bytes <= sizeof(captureBuffer)) {
                        memcpy(captureBuffer, data, bytes);
                    }
                }
                ae_engine_write_capture(engine_, captureBuffer, framesAvailable);
                captureService_->ReleaseBuffer(framesAvailable);
            }
            captureService_->GetNextPacketSize(&packetLength);
        }

        // ─── Process engine ───
        ae_engine_process(engine_);

        // ─── Render ───
        UINT32 padding = 0;
        renderClient_->GetCurrentPadding(&padding);
        UINT32 framesAvailable = renderBufferSize_ - padding;

        if (framesAvailable >= AE_FRAME_SIZE) {
            BYTE *data;
            HRESULT hr = renderService_->GetBuffer(AE_FRAME_SIZE, &data);
            if (SUCCEEDED(hr)) {
                int framesRead = ae_engine_read_playback(engine_, (float *)data, AE_FRAME_SIZE);
                if (framesRead < AE_FRAME_SIZE) {
                    memset((float *)data + framesRead * AE_CHANNELS, 0,
                           (AE_FRAME_SIZE - framesRead) * AE_CHANNELS * sizeof(float));
                }
                renderService_->ReleaseBuffer(AE_FRAME_SIZE, 0);
            }
        }

        // Sleep ~10ms (half frame) to maintain timing
        Sleep(10);
    }
}

void WasapiAudio::cleanup() {
    if (renderClient_) { renderClient_->Stop(); }
    if (captureClient_) { captureClient_->Stop(); }
    if (renderService_) { renderService_->Release(); renderService_ = nullptr; }
    if (captureService_) { captureService_->Release(); captureService_ = nullptr; }
    if (renderClient_) { renderClient_->Release(); renderClient_ = nullptr; }
    if (captureClient_) { captureClient_->Release(); captureClient_ = nullptr; }
    if (renderDevice_) { renderDevice_->Release(); renderDevice_ = nullptr; }
    if (captureDevice_) { captureDevice_->Release(); captureDevice_ = nullptr; }
    if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
    CoUninitialize();
}

#endif // _WIN32
