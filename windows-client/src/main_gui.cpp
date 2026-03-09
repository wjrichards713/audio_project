/**
 * main_gui.cpp - Windows GUI audio client
 *
 * Win32 native GUI with channel controls, PTT buttons,
 * volume sliders, and live status display.
 *
 * Connection flow:
 *   1. WebSocket connect + auth -> get client_id (UUID) and UDP port
 *   2. UDP connect with engine (using client_id for routing headers)
 *   3. All signaling (join/leave/talk/stop) goes over WebSocket
 *   4. Audio data flows over UDP
 */

#ifdef _WIN32

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <string>
#include <mutex>
#include <thread>

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

extern "C" {
#include "audio_engine.h"
}
#include "wasapi_audio.h"
#include "ws_signaling.h"

/* ─── Server Configuration ──────────────────────────────────────── */
#define DEFAULT_SERVER_HOST "34.219.36.101"
#define DEFAULT_UDP_PORT    10000
#define DEFAULT_WS_PORT     8080
#define DEFAULT_AUTH_TOKEN  "windows-client"

/* ─── Channel Data ──────────────────────────────────────────────── */
static const char *CHANNEL_IDS[] = {"channel-1", "channel-2", "channel-3"};
static const wchar_t *CHANNEL_NAMES[] = {L"Operations", L"Dispatch", L"Emergency"};
static bool channelJoined[3] = {};
static bool transmitting[3] = {};
static bool channelMuted[3] = {};

/* ─── Engine State ──────────────────────────────────────────────── */
static ae_engine_t *g_engine = nullptr;
static WasapiAudio *g_audio = nullptr;
static WsSignaling *g_ws = nullptr;
static std::atomic<bool> g_connected{false};
static std::mutex g_logMutex;
static std::wstring g_logText;

/* ─── Window Handles ────────────────────────────────────────────── */
static HWND g_hwnd = nullptr;
static HWND g_statusLabel = nullptr;
static HWND g_connectBtn = nullptr;
static HWND g_disconnectBtn = nullptr;
static HWND g_logBox = nullptr;
static HWND g_masterVolSlider = nullptr;
static HWND g_masterVolLabel = nullptr;
static HWND g_statsLabel = nullptr;

// Per-channel controls
static HWND g_chJoinBtn[3] = {};
static HWND g_chTalkBtn[3] = {};
static HWND g_chMuteBtn[3] = {};
static HWND g_chVolSlider[3] = {};
static HWND g_chVolLabel[3] = {};
static HWND g_chStatusLabel[3] = {};

/* ─── Control IDs ───────────────────────────────────────────────── */
#define ID_CONNECT_BTN      1001
#define ID_DISCONNECT_BTN   1002
#define ID_CH1_JOIN         1010
#define ID_CH2_JOIN         1011
#define ID_CH3_JOIN         1012
#define ID_CH1_TALK         1020
#define ID_CH2_TALK         1021
#define ID_CH3_TALK         1022
#define ID_CH1_MUTE         1030
#define ID_CH2_MUTE         1031
#define ID_CH3_MUTE         1032
#define ID_MASTER_VOL       1040
#define ID_CH1_VOL          1041
#define ID_CH2_VOL          1042
#define ID_CH3_VOL          1043
#define ID_TIMER_STATS      2001

/* ─── Colors ────────────────────────────────────────────────────── */
#define CLR_BG          RGB(30, 30, 30)
#define CLR_PANEL       RGB(45, 45, 48)
#define CLR_TEXT        RGB(220, 220, 220)
#define CLR_GREEN       RGB(76, 175, 80)
#define CLR_RED         RGB(244, 67, 54)
#define CLR_ORANGE      RGB(255, 152, 0)
#define CLR_BLUE        RGB(33, 150, 243)
#define CLR_DIMTEXT     RGB(140, 140, 140)

static HBRUSH g_bgBrush = nullptr;
static HBRUSH g_panelBrush = nullptr;
static HFONT g_font = nullptr;
static HFONT g_fontBold = nullptr;
static HFONT g_fontSmall = nullptr;

/* ─── Logging ───────────────────────────────────────────────────── */
static void appendLog(const wchar_t *msg) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logText += msg;
    g_logText += L"\r\n";
    if (g_logBox) {
        SetWindowTextW(g_logBox, g_logText.c_str());
        SendMessageW(g_logBox, EM_SETSEL, g_logText.size(), g_logText.size());
        SendMessageW(g_logBox, EM_SCROLLCARET, 0, 0);
    }
}

static void appendLogA(const char *msg) {
    wchar_t buf[512];
    MultiByteToWideChar(CP_UTF8, 0, msg, -1, buf, 512);
    appendLog(buf);
}

/* ─── WebSocket Event Handler ───────────────────────────────────── */
static void onWsEvent(const WsEvent &ev) {
    wchar_t buf[256];

    if (ev.type == "user_joined") {
        swprintf(buf, 256, L"[WS] User '%hs' joined %hs",
                 ev.user_name.c_str(), ev.channel_id.c_str());
        appendLog(buf);
    }
    else if (ev.type == "user_left") {
        swprintf(buf, 256, L"[WS] User '%hs' left %hs",
                 ev.client_id.c_str(), ev.channel_id.c_str());
        appendLog(buf);
    }
    else if (ev.type == "user_speaking") {
        swprintf(buf, 256, L"[WS] User speaking in %hs", ev.channel_id.c_str());
        appendLog(buf);
    }
    else if (ev.type == "user_stopped") {
        swprintf(buf, 256, L"[WS] User stopped in %hs", ev.channel_id.c_str());
        appendLog(buf);
    }
    else if (ev.type == "channel_joined") {
        swprintf(buf, 256, L"[WS] Server confirmed join: %hs", ev.channel_id.c_str());
        appendLog(buf);
    }
    else if (ev.type == "channel_left") {
        swprintf(buf, 256, L"[WS] Server confirmed leave: %hs", ev.channel_id.c_str());
        appendLog(buf);
    }
    else if (ev.type == "error") {
        swprintf(buf, 256, L"[WS] Server error: %hs", ev.message.c_str());
        appendLog(buf);
    }
    else if (ev.type == "pong") {
        // Could calculate RTT here
    }
    else if (ev.type == "disconnected") {
        g_connected.store(false);
        appendLog(L"[WS] Connection lost");
        if (g_hwnd) PostMessage(g_hwnd, WM_APP + 1, 0, 0);
    }
}

/* ─── UI Update Helpers ─────────────────────────────────────────── */
static void updateConnectionUI() {
    bool connected = g_connected.load();
    EnableWindow(g_connectBtn, !connected);
    EnableWindow(g_disconnectBtn, connected);
    SetWindowTextW(g_statusLabel, connected ?
        L"  Status: CONNECTED" : L"  Status: DISCONNECTED");
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void updateChannelUI(int idx) {
    if (channelJoined[idx]) {
        SetWindowTextW(g_chJoinBtn[idx], L"Leave");
        EnableWindow(g_chTalkBtn[idx], TRUE);
        EnableWindow(g_chMuteBtn[idx], TRUE);
    } else {
        SetWindowTextW(g_chJoinBtn[idx], L"Join");
        EnableWindow(g_chTalkBtn[idx], FALSE);
        EnableWindow(g_chMuteBtn[idx], FALSE);
    }
    SetWindowTextW(g_chTalkBtn[idx], transmitting[idx] ? L"Stop TX" : L"Talk");

    wchar_t status[64];
    if (!channelJoined[idx]) {
        wcscpy(status, L"Not joined");
    } else if (transmitting[idx]) {
        wcscpy(status, L"TRANSMITTING");
    } else if (channelMuted[idx]) {
        wcscpy(status, L"Joined (Muted)");
    } else {
        wcscpy(status, L"Joined");
    }
    SetWindowTextW(g_chStatusLabel[idx], status);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void updateStats() {
    if (!g_engine) return;
    ae_stats_t stats = ae_engine_get_stats(g_engine);
    wchar_t buf[256];
    swprintf(buf, 256,
        L"RTT: %.0fms  |  Jitter: %.1fms  |  Loss: %.1f%%  |  Streams: %d  |  Sent: %llu  |  Recv: %llu",
        stats.rtt_ms, stats.jitter_ms, stats.packet_loss_pct,
        stats.active_streams,
        (unsigned long long)stats.packets_sent,
        (unsigned long long)stats.packets_received);
    SetWindowTextW(g_statsLabel, buf);
}

/* ─── Create UI Controls ───────────────────────────────────────── */
static HWND createLabel(HWND parent, const wchar_t *text, int x, int y, int w, int h, HFONT font) {
    HWND hw = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                            x, y, w, h, parent, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessage(hw, WM_SETFONT, (WPARAM)font, TRUE);
    return hw;
}

static HWND createButton(HWND parent, const wchar_t *text, int id, int x, int y, int w, int h) {
    HWND hw = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            x, y, w, h, parent, (HMENU)(intptr_t)id, GetModuleHandle(nullptr), nullptr);
    SendMessage(hw, WM_SETFONT, (WPARAM)g_font, TRUE);
    return hw;
}

static HWND createSlider(HWND parent, int id, int x, int y, int w, int h) {
    HWND hw = CreateWindowW(TRACKBAR_CLASSW, L"",
                            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                            x, y, w, h, parent, (HMENU)(intptr_t)id, GetModuleHandle(nullptr), nullptr);
    SendMessage(hw, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessage(hw, TBM_SETPOS, TRUE, 100);
    return hw;
}

static void createUI(HWND hwnd) {
    int y = 10;
    int lm = 15; // left margin

    // ─── Title ───
    createLabel(hwnd, L"Audio Platform Client", lm, y, 400, 28, g_fontBold);
    y += 35;

    // ─── Connection Panel ───
    g_statusLabel = createLabel(hwnd, L"  Status: DISCONNECTED", lm, y, 300, 22, g_font);
    y += 28;

    g_connectBtn = createButton(hwnd, L"Connect", ID_CONNECT_BTN, lm, y, 120, 32);
    g_disconnectBtn = createButton(hwnd, L"Disconnect", ID_DISCONNECT_BTN, lm + 130, y, 120, 32);
    EnableWindow(g_disconnectBtn, FALSE);
    y += 45;

    // ─── Master Volume ───
    createLabel(hwnd, L"Master Volume:", lm, y + 2, 110, 20, g_font);
    g_masterVolSlider = createSlider(hwnd, ID_MASTER_VOL, lm + 115, y, 200, 25);
    g_masterVolLabel = createLabel(hwnd, L"100%", lm + 320, y + 2, 50, 20, g_font);
    y += 38;

    // ─── Separator ───
    y += 5;

    // ─── Channel Panels ───
    for (int i = 0; i < 3; i++) {
        int panelY = y;
        int cx = lm + 5;

        // Channel name header
        wchar_t chTitle[64];
        swprintf(chTitle, 64, L"Ch %d: %s", i + 1, CHANNEL_NAMES[i]);
        createLabel(hwnd, chTitle, cx, panelY, 200, 22, g_fontBold);

        // Status label
        g_chStatusLabel[i] = createLabel(hwnd, L"Not joined", cx + 210, panelY, 160, 22, g_fontSmall);
        panelY += 28;

        // Buttons
        g_chJoinBtn[i] = createButton(hwnd, L"Join", ID_CH1_JOIN + i, cx, panelY, 80, 30);
        g_chTalkBtn[i] = createButton(hwnd, L"Talk", ID_CH1_TALK + i, cx + 90, panelY, 80, 30);
        g_chMuteBtn[i] = createButton(hwnd, L"Mute", ID_CH1_MUTE + i, cx + 180, panelY, 80, 30);

        EnableWindow(g_chTalkBtn[i], FALSE);
        EnableWindow(g_chMuteBtn[i], FALSE);

        // Volume slider
        createLabel(hwnd, L"Vol:", cx + 275, panelY + 5, 30, 20, g_fontSmall);
        g_chVolSlider[i] = createSlider(hwnd, ID_CH1_VOL + i, cx + 305, panelY + 2, 130, 25);
        g_chVolLabel[i] = createLabel(hwnd, L"100%", cx + 440, panelY + 5, 50, 20, g_fontSmall);

        panelY += 40;
        y = panelY + 10;
    }

    // ─── Stats Bar ───
    g_statsLabel = createLabel(hwnd, L"RTT: --  |  Jitter: --  |  Loss: --  |  Streams: 0", lm, y, 550, 20, g_fontSmall);
    y += 28;

    // ─── Event Log ───
    createLabel(hwnd, L"Event Log:", lm, y, 100, 20, g_font);
    y += 22;
    g_logBox = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        lm, y, 550, 150,
        hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessage(g_logBox, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
}

/* ─── Connect (WS + UDP) on background thread ──────────────────── */
static void doConnect() {
    appendLog(L"Connecting WebSocket...");

    if (!g_ws) g_ws = new WsSignaling();
    g_ws->setEventCallback(onWsEvent);

    if (!g_ws->connect(DEFAULT_SERVER_HOST, DEFAULT_WS_PORT, DEFAULT_AUTH_TOKEN)) {
        appendLog(L"WebSocket connection failed!");
        PostMessage(g_hwnd, WM_APP + 1, 0, 0);
        return;
    }

    wchar_t buf[256];
    swprintf(buf, 256, L"Authenticated: client=%hs (UDP ID: %llu)",
             g_ws->getClientId().c_str(),
             (unsigned long long)g_ws->getUdpClientId());
    appendLog(buf);

    // Set the client_id on the engine so UDP packets use the correct routing ID
    ae_engine_set_client_id(g_engine, g_ws->getUdpClientId());

    // Now connect UDP
    appendLog(L"Connecting UDP...");
    int result = ae_engine_connect(g_engine, DEFAULT_SERVER_HOST,
                                    DEFAULT_UDP_PORT, DEFAULT_WS_PORT,
                                    DEFAULT_AUTH_TOKEN);
    if (result != AE_OK) {
        swprintf(buf, 256, L"UDP connect failed: error %d", result);
        appendLog(buf);
        g_ws->disconnect();
        PostMessage(g_hwnd, WM_APP + 1, 0, 0);
        return;
    }

    g_connected.store(true);
    appendLog(L"Connected! (WS + UDP)");
    PostMessage(g_hwnd, WM_APP + 1, 0, 0);
}

/* ─── Window Procedure ──────────────────────────────────────────── */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        createUI(hwnd);
        SetTimer(hwnd, ID_TIMER_STATS, 500, nullptr);
        return 0;

    case WM_TIMER:
        if (wParam == ID_TIMER_STATS) {
            updateStats();
        }
        return 0;

    case WM_APP + 1: // Connection state changed
        updateConnectionUI();
        return 0;

    case WM_HSCROLL: {
        HWND slider = (HWND)lParam;
        int pos = (int)SendMessage(slider, TBM_GETPOS, 0, 0);
        wchar_t buf[16];
        swprintf(buf, 16, L"%d%%", pos);
        float fvol = pos / 100.0f;

        if (slider == g_masterVolSlider) {
            SetWindowTextW(g_masterVolLabel, buf);
            if (g_engine) ae_engine_set_master_volume(g_engine, fvol);
        }
        for (int i = 0; i < 3; i++) {
            if (slider == g_chVolSlider[i]) {
                SetWindowTextW(g_chVolLabel[i], buf);
                if (g_engine) ae_engine_set_channel_volume(g_engine, CHANNEL_IDS[i], fvol);
            }
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);

        if (id == ID_CONNECT_BTN) {
            if (g_engine && !g_connected.load()) {
                EnableWindow(g_connectBtn, FALSE);
                // Connect on background thread to avoid blocking UI
                std::thread(doConnect).detach();
            }
        }
        else if (id == ID_DISCONNECT_BTN) {
            if (g_engine) {
                ae_engine_disconnect(g_engine);
                if (g_ws) g_ws->disconnect();
                for (int i = 0; i < 3; i++) {
                    channelJoined[i] = false;
                    transmitting[i] = false;
                    channelMuted[i] = false;
                    updateChannelUI(i);
                }
                g_connected.store(false);
                updateConnectionUI();
                appendLog(L"Disconnected");
            }
        }
        else if (id >= ID_CH1_JOIN && id <= ID_CH3_JOIN) {
            int idx = id - ID_CH1_JOIN;
            if (!channelJoined[idx]) {
                // Join via WebSocket signaling
                if (g_ws && g_ws->joinChannel(CHANNEL_IDS[idx])) {
                    // Also tell the engine (for local audio state)
                    ae_engine_join_channel(g_engine, CHANNEL_IDS[idx]);
                    channelJoined[idx] = true;
                    wchar_t buf[128];
                    swprintf(buf, 128, L"Joining %s...", CHANNEL_NAMES[idx]);
                    appendLog(buf);
                }
            } else {
                // Leave via WebSocket
                if (g_ws) g_ws->leaveChannel(CHANNEL_IDS[idx]);
                ae_engine_leave_channel(g_engine, CHANNEL_IDS[idx]);
                channelJoined[idx] = false;
                transmitting[idx] = false;
                channelMuted[idx] = false;
                wchar_t buf[128];
                swprintf(buf, 128, L"Left %s", CHANNEL_NAMES[idx]);
                appendLog(buf);
            }
            updateChannelUI(idx);
        }
        else if (id >= ID_CH1_TALK && id <= ID_CH3_TALK) {
            int idx = id - ID_CH1_TALK;
            if (!transmitting[idx]) {
                // Start transmit via WS + engine
                if (g_ws) g_ws->startTransmit(CHANNEL_IDS[idx]);
                ae_engine_start_transmit(g_engine, CHANNEL_IDS[idx]);
                transmitting[idx] = true;
                wchar_t buf[128];
                swprintf(buf, 128, L"Transmitting on %s", CHANNEL_NAMES[idx]);
                appendLog(buf);
            } else {
                // Stop transmit via WS + engine
                if (g_ws) g_ws->stopTransmit(CHANNEL_IDS[idx]);
                ae_engine_stop_transmit(g_engine, CHANNEL_IDS[idx]);
                transmitting[idx] = false;
                wchar_t buf[128];
                swprintf(buf, 128, L"Stopped TX on %s", CHANNEL_NAMES[idx]);
                appendLog(buf);
            }
            updateChannelUI(idx);
        }
        else if (id >= ID_CH1_MUTE && id <= ID_CH3_MUTE) {
            int idx = id - ID_CH1_MUTE;
            channelMuted[idx] = !channelMuted[idx];
            ae_engine_set_channel_muted(g_engine, CHANNEL_IDS[idx], channelMuted[idx]);
            SetWindowTextW(g_chMuteBtn[idx], channelMuted[idx] ? L"Unmute" : L"Mute");
            wchar_t buf[128];
            swprintf(buf, 128, L"%s %s", channelMuted[idx] ? L"Muted" : L"Unmuted", CHANNEL_NAMES[idx]);
            appendLog(buf);
            updateChannelUI(idx);
        }
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, CLR_TEXT);
        SetBkColor(hdc, CLR_BG);
        return (LRESULT)g_bgBrush;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, CLR_TEXT);
        SetBkColor(hdc, CLR_PANEL);
        return (LRESULT)g_panelBrush;
    }

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_bgBrush);
        return 1;
    }

    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER_STATS);
        PostQuitMessage(0);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ─── Entry Point ───────────────────────────────────────────────── */
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    // Init Winsock early
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // Init common controls for sliders
    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    // Create fonts
    g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontBold = CreateFontW(-16, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                              0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontSmall = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                               0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    // Create brushes
    g_bgBrush = CreateSolidBrush(CLR_BG);
    g_panelBrush = CreateSolidBrush(CLR_PANEL);

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_bgBrush;
    wc.lpszClassName = L"AudioPlatformClient";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // Create window
    int winW = 610, winH = 620;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    g_hwnd = CreateWindowExW(0, L"AudioPlatformClient",
        L"Audio Platform Client",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        (screenW - winW) / 2, (screenH - winH) / 2,
        winW, winH, nullptr, nullptr, hInstance, nullptr);

    if (!g_hwnd) return 1;

    // Create audio engine
    ae_config_t config = ae_config_default();
    g_engine = ae_engine_create(&config);
    if (!g_engine) {
        MessageBoxW(g_hwnd, L"Failed to create audio engine", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Start WASAPI audio
    g_audio = new WasapiAudio(g_engine);
    if (g_audio->start() != 0) {
        appendLog(L"Warning: Audio I/O failed to start. Signaling only.");
    } else {
        appendLog(L"Audio engine started (48kHz stereo float32)");
    }
    appendLog(L"Server: " L"" DEFAULT_SERVER_HOST L" (WS:8080 UDP:10000)");
    appendLog(L"Click Connect to start.");

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Cleanup
    if (g_ws) {
        g_ws->disconnect();
        delete g_ws;
    }
    if (g_audio) {
        g_audio->stop();
        delete g_audio;
    }
    if (g_engine) {
        ae_engine_disconnect(g_engine);
        ae_engine_destroy(g_engine);
    }

    DeleteObject(g_font);
    DeleteObject(g_fontBold);
    DeleteObject(g_fontSmall);
    DeleteObject(g_bgBrush);
    DeleteObject(g_panelBrush);

    WSACleanup();
    return (int)msg.wParam;
}

#endif // _WIN32
