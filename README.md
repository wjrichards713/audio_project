# Audio Platform - Real-Time Multi-Channel Audio

A real-time audio communication platform with music-quality sound (48kHz stereo, Opus 128kbps).

## Architecture

```
┌──────────────┐     ┌─────────────┐     ┌──────────────────┐
│ Android App  │────▶│  Rust SFU   │◀────│ Windows Client   │
│ (Oboe/AAudio)│     │  Server     │     │ (WASAPI)         │
└──────┬───────┘     └──────┬──────┘     └──────┬───────────┘
       │                    │                    │
       └────────┬───────────┘────────────────────┘
                │
         ┌──────┴──────┐
         │ Shared C    │
         │ Core Engine │
         │ (Opus,RTP,  │
         │  Jitter,Mix)│
         └─────────────┘
```

## Components

### `/core` - Shared C Audio Engine
Cross-platform C library compiled on all platforms. Contains:
- Opus codec (128kbps, 48kHz stereo, music mode)
- Lock-free jitter buffer
- Float32 audio mixer with soft limiter
- AES-256-GCM encryption
- Custom RTP with FEC
- Ring buffers for audio thread safety

### `/server` - Rust SFU Server
Selective Forwarding Unit that routes encrypted audio packets without decoding:
- Tokio async runtime
- UDP audio forwarding
- WebSocket signaling (JSON)
- 3 pre-configured channels

### `/android-app` - Android Client
Native Android app using Oboe/AAudio for low-latency audio:
- Kotlin UI with PTT, volume controls, 3 channels
- JNI bridge to C core engine
- 48kHz float32 exclusive mode (fixes resampling pops)

### `/windows-client` - Windows Desktop Client
Console application using WASAPI:
- Shared mode 48kHz float32 stereo
- Interactive CLI with PTT, channel management
- Same C core engine

## Key Audio Settings
| Parameter | Value |
|-----------|-------|
| Sample Rate | 48,000 Hz |
| Channels | 2 (stereo) |
| Format | float32 |
| Frame Size | 960 samples (20ms) |
| Opus Bitrate | 128 kbps |
| Opus Application | AUDIO (not VOIP) |
| FEC | Enabled |

## Building

### Server (Rust)
```bash
cd server
cargo build --release
```

### Android App
Open `android-app/` in Android Studio. NDK and CMake required.
The Gradle build will fetch Oboe and Opus automatically.

### Windows Client
```bash
cd windows-client
mkdir build && cd build
cmake ..
cmake --build . --config Release
```

## Proof of Concept Scope
- 1 server instance
- 3 channels (Operations, Dispatch, Emergency)
- Android + Windows clients
- Push-to-talk transmission
- Per-channel and master volume
