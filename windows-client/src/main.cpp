/**
 * main.cpp - Windows desktop audio client
 *
 * Console application for testing the audio engine with
 * 3 channels, PTT, and volume control via keyboard.
 */

#ifdef _WIN32

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <string>

extern "C" {
#include "audio_engine.h"
}
#include "wasapi_audio.h"

static std::atomic<bool> g_running{true};
static ae_engine_t *g_engine = nullptr;
static WasapiAudio *g_audio = nullptr;

// Channel names matching the Android client
static const char *CHANNEL_IDS[] = {"channel-1", "channel-2", "channel-3"};
static const char *CHANNEL_NAMES[] = {"Operations", "Dispatch", "Emergency"};
static bool channelJoined[3] = {false, false, false};
static bool transmitting[3] = {false, false, false};

static void eventCallback(const ae_event_t *event, void * /*user_data*/) {
    switch (event->type) {
        case AE_EVENT_CONNECTED:
            printf("\n[+] Connected to server\n> ");
            fflush(stdout);
            break;
        case AE_EVENT_DISCONNECTED:
            printf("\n[-] Disconnected from server\n> ");
            fflush(stdout);
            break;
        case AE_EVENT_USER_JOINED:
            printf("\n[+] User '%s' joined %s\n> ",
                   event->user_name ? event->user_name : "unknown",
                   event->channel_id ? event->channel_id : "?");
            fflush(stdout);
            break;
        case AE_EVENT_USER_LEFT:
            printf("\n[-] User left %s (client=%s)\n> ",
                   event->channel_id ? event->channel_id : "?",
                   event->client_id ? event->client_id : "?");
            fflush(stdout);
            break;
        case AE_EVENT_USER_SPEAKING:
            printf("\n[~] User speaking in %s\n> ",
                   event->channel_id ? event->channel_id : "?");
            fflush(stdout);
            break;
        case AE_EVENT_USER_STOPPED:
            printf("\n[.] User stopped in %s\n> ",
                   event->channel_id ? event->channel_id : "?");
            fflush(stdout);
            break;
        case AE_EVENT_ERROR:
            printf("\n[!] Error: %s\n> ",
                   event->message ? event->message : "unknown");
            fflush(stdout);
            break;
        default:
            break;
    }
}

static BOOL WINAPI consoleHandler(DWORD /*signal*/) {
    g_running.store(false);
    return TRUE;
}

static void printHelp() {
    printf("\n");
    printf("=== Audio Platform - Windows Client ===\n");
    printf("\n");
    printf("Commands:\n");
    printf("  connect <host> [udp_port] [ws_port] [token]\n");
    printf("      Connect to server (default ports: 7000 UDP, 7001 WS)\n");
    printf("  disconnect\n");
    printf("      Disconnect from server\n");
    printf("  join <1|2|3>\n");
    printf("      Join channel (1=Operations, 2=Dispatch, 3=Emergency)\n");
    printf("  leave <1|2|3>\n");
    printf("      Leave channel\n");
    printf("  talk <1|2|3>\n");
    printf("      Start transmitting on channel (PTT on)\n");
    printf("  stop <1|2|3>\n");
    printf("      Stop transmitting on channel (PTT off)\n");
    printf("  vol <0-100>\n");
    printf("      Set master volume\n");
    printf("  chvol <1|2|3> <0-100>\n");
    printf("      Set channel volume\n");
    printf("  mute <1|2|3>\n");
    printf("      Toggle channel mute\n");
    printf("  stats\n");
    printf("      Show connection statistics\n");
    printf("  status\n");
    printf("      Show channel status\n");
    printf("  help\n");
    printf("      Show this help\n");
    printf("  quit\n");
    printf("      Exit\n");
    printf("\n");
}

static void printStatus() {
    bool connected = g_engine && ae_engine_is_connected(g_engine);
    printf("\nConnection: %s\n", connected ? "CONNECTED" : "DISCONNECTED");
    for (int i = 0; i < 3; i++) {
        printf("  Channel %d (%s): %s%s\n",
               i + 1, CHANNEL_NAMES[i],
               channelJoined[i] ? "JOINED" : "not joined",
               transmitting[i] ? " [TRANSMITTING]" : "");
    }
    if (connected) {
        ae_stats_t stats = ae_engine_get_stats(g_engine);
        printf("  RTT: %.1f ms | Loss: %.1f%% | Streams: %d\n",
               stats.rtt_ms, stats.packet_loss_pct, stats.active_streams);
    }
    printf("\n");
}

static void printStats() {
    if (!g_engine) {
        printf("Engine not created\n");
        return;
    }
    ae_stats_t stats = ae_engine_get_stats(g_engine);
    printf("\n--- Statistics ---\n");
    printf("  RTT:              %.1f ms\n", stats.rtt_ms);
    printf("  Jitter:           %.1f ms\n", stats.jitter_ms);
    printf("  Packet loss:      %.1f%%\n", stats.packet_loss_pct);
    printf("  Buffer underruns: %d\n", stats.buffer_underruns);
    printf("  Active streams:   %d\n", stats.active_streams);
    printf("  Joined channels:  %d\n", stats.joined_channels);
    printf("  Packets sent:     %llu\n", (unsigned long long)stats.packets_sent);
    printf("  Packets received: %llu\n", (unsigned long long)stats.packets_received);
    printf("\n");
}

int main(int /*argc*/, char * /*argv*/[]) {
    SetConsoleCtrlHandler(consoleHandler, TRUE);
    SetConsoleOutputCP(CP_UTF8);

    printf("Audio Platform - Windows Client v1.0\n");
    printf("48kHz stereo float32 | Opus 128kbps | 20ms frames\n");
    printf("Type 'help' for commands.\n\n");

    // Create engine
    ae_config_t config = ae_config_default();
    g_engine = ae_engine_create(&config);
    if (!g_engine) {
        fprintf(stderr, "Failed to create audio engine\n");
        return 1;
    }

    ae_engine_set_event_callback(g_engine, eventCallback, nullptr);

    // Start WASAPI audio
    g_audio = new WasapiAudio(g_engine);
    if (g_audio->start() != 0) {
        fprintf(stderr, "Warning: Failed to start audio I/O\n");
        fprintf(stderr, "Audio will not work but you can still test signaling.\n");
    }

    char line[512];

    while (g_running.load()) {
        printf("> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;

        // Strip newline
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (len > 1 && line[len - 2] == '\r') line[len - 2] = '\0';

        if (line[0] == '\0') continue;

        char cmd[64] = {};
        char arg1[256] = {};
        char arg2[256] = {};
        char arg3[256] = {};
        char arg4[256] = {};
        sscanf(line, "%63s %255s %255s %255s %255s", cmd, arg1, arg2, arg3, arg4);

        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0 || strcmp(cmd, "q") == 0) {
            break;
        }
        else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
            printHelp();
        }
        else if (strcmp(cmd, "connect") == 0) {
            if (arg1[0] == '\0') {
                printf("Usage: connect <host> [udp_port] [ws_port] [token]\n");
                continue;
            }
            int udpPort = arg2[0] ? atoi(arg2) : 7000;
            int wsPort = arg3[0] ? atoi(arg3) : 7001;
            const char *token = arg4[0] ? arg4 : "test-token";

            printf("Connecting to %s (UDP:%d WS:%d)...\n", arg1, udpPort, wsPort);
            int result = ae_engine_connect(g_engine, arg1, udpPort, wsPort, token);
            if (result != AE_OK) {
                printf("Connect failed: error %d\n", result);
            }
        }
        else if (strcmp(cmd, "disconnect") == 0) {
            ae_engine_disconnect(g_engine);
            for (int i = 0; i < 3; i++) {
                channelJoined[i] = false;
                transmitting[i] = false;
            }
            printf("Disconnected\n");
        }
        else if (strcmp(cmd, "join") == 0) {
            int ch = atoi(arg1);
            if (ch < 1 || ch > 3) {
                printf("Usage: join <1|2|3>\n");
                continue;
            }
            int idx = ch - 1;
            int result = ae_engine_join_channel(g_engine, CHANNEL_IDS[idx]);
            if (result == AE_OK) {
                channelJoined[idx] = true;
                printf("Joined %s (%s)\n", CHANNEL_NAMES[idx], CHANNEL_IDS[idx]);
            } else {
                printf("Failed to join: error %d\n", result);
            }
        }
        else if (strcmp(cmd, "leave") == 0) {
            int ch = atoi(arg1);
            if (ch < 1 || ch > 3) {
                printf("Usage: leave <1|2|3>\n");
                continue;
            }
            int idx = ch - 1;
            ae_engine_leave_channel(g_engine, CHANNEL_IDS[idx]);
            channelJoined[idx] = false;
            transmitting[idx] = false;
            printf("Left %s\n", CHANNEL_NAMES[idx]);
        }
        else if (strcmp(cmd, "talk") == 0) {
            int ch = atoi(arg1);
            if (ch < 1 || ch > 3) {
                printf("Usage: talk <1|2|3>\n");
                continue;
            }
            int idx = ch - 1;
            if (!channelJoined[idx]) {
                printf("Not joined to %s. Join first.\n", CHANNEL_NAMES[idx]);
                continue;
            }
            int result = ae_engine_start_transmit(g_engine, CHANNEL_IDS[idx]);
            if (result == AE_OK) {
                transmitting[idx] = true;
                printf("Transmitting on %s (type 'stop %d' to stop)\n", CHANNEL_NAMES[idx], ch);
            } else {
                printf("Failed to start transmit: error %d\n", result);
            }
        }
        else if (strcmp(cmd, "stop") == 0) {
            int ch = atoi(arg1);
            if (ch < 1 || ch > 3) {
                printf("Usage: stop <1|2|3>\n");
                continue;
            }
            int idx = ch - 1;
            ae_engine_stop_transmit(g_engine, CHANNEL_IDS[idx]);
            transmitting[idx] = false;
            printf("Stopped transmitting on %s\n", CHANNEL_NAMES[idx]);
        }
        else if (strcmp(cmd, "vol") == 0) {
            int vol = atoi(arg1);
            if (vol < 0 || vol > 100) {
                printf("Usage: vol <0-100>\n");
                continue;
            }
            float fvol = vol / 100.0f;
            ae_engine_set_master_volume(g_engine, fvol);
            printf("Master volume: %d%%\n", vol);
        }
        else if (strcmp(cmd, "chvol") == 0) {
            int ch = atoi(arg1);
            int vol = atoi(arg2);
            if (ch < 1 || ch > 3 || vol < 0 || vol > 100) {
                printf("Usage: chvol <1|2|3> <0-100>\n");
                continue;
            }
            float fvol = vol / 100.0f;
            ae_engine_set_channel_volume(g_engine, CHANNEL_IDS[ch - 1], fvol);
            printf("%s volume: %d%%\n", CHANNEL_NAMES[ch - 1], vol);
        }
        else if (strcmp(cmd, "mute") == 0) {
            int ch = atoi(arg1);
            if (ch < 1 || ch > 3) {
                printf("Usage: mute <1|2|3>\n");
                continue;
            }
            // Toggle mute (simple toggle — no state tracking needed for POC)
            ae_engine_set_channel_muted(g_engine, CHANNEL_IDS[ch - 1], true);
            printf("Toggled mute on %s\n", CHANNEL_NAMES[ch - 1]);
        }
        else if (strcmp(cmd, "stats") == 0) {
            printStats();
        }
        else if (strcmp(cmd, "status") == 0) {
            printStatus();
        }
        else {
            printf("Unknown command: %s (type 'help')\n", cmd);
        }
    }

    // Cleanup
    printf("\nShutting down...\n");
    if (g_audio) {
        g_audio->stop();
        delete g_audio;
        g_audio = nullptr;
    }
    if (g_engine) {
        ae_engine_disconnect(g_engine);
        ae_engine_destroy(g_engine);
        g_engine = nullptr;
    }

    printf("Goodbye.\n");
    return 0;
}

#else
// Non-Windows stub
#include <cstdio>
int main() {
    fprintf(stderr, "This client requires Windows (WASAPI).\n");
    return 1;
}
#endif
