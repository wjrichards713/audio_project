/**
 * ws_signaling.cpp - Minimal WebSocket signaling client
 *
 * Implements just enough of the WebSocket protocol (RFC 6455) to
 * exchange JSON text frames with the audio SFU server.
 */

#ifdef _WIN32

#include "ws_signaling.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

/* ─── Simple JSON helpers (no external dependency) ──────────────── */

// Extract a string value for a given key from JSON
static std::string jsonGetString(const std::string &json, const char *key) {
    std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    size_t end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}

// Extract a numeric value for a given key
static uint64_t jsonGetUint64(const std::string &json, const char *key) {
    std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return 0;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    return strtoull(json.c_str() + pos, nullptr, 10);
}

/* ─── WebSocket frame helpers ───────────────────────────────────── */

// Generate a random 4-byte mask key
static void randomMask(uint8_t mask[4]) {
    static bool seeded = false;
    if (!seeded) { srand((unsigned)time(nullptr)); seeded = true; }
    for (int i = 0; i < 4; i++) mask[i] = (uint8_t)(rand() & 0xFF);
}

// Base64 encode (for WebSocket handshake key)
static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string base64Encode(const uint8_t *data, size_t len) {
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i + 1 < len) n |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) n |= data[i + 2];
        out += b64chars[(n >> 18) & 0x3F];
        out += b64chars[(n >> 12) & 0x3F];
        out += (i + 1 < len) ? b64chars[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? b64chars[n & 0x3F] : '=';
    }
    return out;
}

/* ─── TCP send/recv helpers ─────────────────────────────────────── */

static bool sendAll(SOCKET s, const void *buf, int len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        int sent = send(s, p, len, 0);
        if (sent <= 0) return false;
        p += sent;
        len -= sent;
    }
    return true;
}

static bool recvAll(SOCKET s, void *buf, int len) {
    char *p = (char *)buf;
    while (len > 0) {
        int got = recv(s, p, len, 0);
        if (got <= 0) return false;
        p += got;
        len -= got;
    }
    return true;
}

// Read until \r\n\r\n (HTTP response end)
static bool recvHttpResponse(SOCKET s, std::string &out) {
    out.clear();
    char c;
    while (out.size() < 4096) {
        if (recv(s, &c, 1, 0) != 1) return false;
        out += c;
        if (out.size() >= 4 && out.substr(out.size() - 4) == "\r\n\r\n")
            return true;
    }
    return false;
}

/* ─── WsSignaling implementation ────────────────────────────────── */

WsSignaling::WsSignaling() {}

WsSignaling::~WsSignaling() {
    disconnect();
}

bool WsSignaling::doHandshake(const char *host, uint16_t port) {
    // Resolve and connect TCP
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%u", port);

    if (getaddrinfo(host, portStr, &hints, &res) != 0 || !res)
        return false;

    sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock_ == INVALID_SOCKET) {
        freeaddrinfo(res);
        return false;
    }

    if (::connect(sock_, res->ai_addr, (int)res->ai_addrlen) != 0) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    // Generate random WebSocket key
    uint8_t keyBytes[16];
    randomMask(keyBytes);
    randomMask(keyBytes + 4);
    randomMask(keyBytes + 8);
    randomMask(keyBytes + 12);
    std::string wsKey = base64Encode(keyBytes, 16);

    // Send HTTP upgrade request
    char request[512];
    snprintf(request, sizeof(request),
        "GET / HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        host, port, wsKey.c_str());

    if (!sendAll(sock_, request, (int)strlen(request)))
        return false;

    // Read HTTP 101 response
    std::string response;
    if (!recvHttpResponse(sock_, response))
        return false;

    if (response.find("101") == std::string::npos) {
        fprintf(stderr, "WS handshake failed: %s\n", response.c_str());
        return false;
    }

    return true;
}

bool WsSignaling::sendWsText(const std::string &text) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (sock_ == INVALID_SOCKET) return false;

    size_t len = text.size();
    std::vector<uint8_t> frame;

    // Opcode 0x81 = FIN + Text
    frame.push_back(0x81);

    // Payload length with mask bit
    if (len < 126) {
        frame.push_back((uint8_t)(0x80 | len));
    } else if (len < 65536) {
        frame.push_back(0x80 | 126);
        frame.push_back((uint8_t)(len >> 8));
        frame.push_back((uint8_t)(len & 0xFF));
    } else {
        return false; // Messages > 64K not needed
    }

    // Mask key
    uint8_t mask[4];
    randomMask(mask);
    frame.push_back(mask[0]);
    frame.push_back(mask[1]);
    frame.push_back(mask[2]);
    frame.push_back(mask[3]);

    // Masked payload
    for (size_t i = 0; i < len; i++) {
        frame.push_back(text[i] ^ mask[i % 4]);
    }

    return sendAll(sock_, frame.data(), (int)frame.size());
}

bool WsSignaling::recvWsMessage(std::string &out) {
    out.clear();
    if (sock_ == INVALID_SOCKET) return false;

    // Read first 2 bytes
    uint8_t header[2];
    if (!recvAll(sock_, header, 2)) return false;

    uint8_t opcode = header[0] & 0x0F;
    bool masked = (header[1] & 0x80) != 0;
    uint64_t payloadLen = header[1] & 0x7F;

    if (payloadLen == 126) {
        uint8_t ext[2];
        if (!recvAll(sock_, ext, 2)) return false;
        payloadLen = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (payloadLen == 127) {
        uint8_t ext[8];
        if (!recvAll(sock_, ext, 8)) return false;
        payloadLen = 0;
        for (int i = 0; i < 8; i++)
            payloadLen = (payloadLen << 8) | ext[i];
    }

    uint8_t mask[4] = {};
    if (masked) {
        if (!recvAll(sock_, mask, 4)) return false;
    }

    if (payloadLen > 1024 * 1024) return false; // Safety limit

    std::vector<char> payload((size_t)payloadLen);
    if (payloadLen > 0) {
        if (!recvAll(sock_, payload.data(), (int)payloadLen)) return false;
        if (masked) {
            for (size_t i = 0; i < payloadLen; i++)
                payload[i] ^= mask[i % 4];
        }
    }

    // Handle close frame
    if (opcode == 0x08) {
        return false;
    }
    // Handle ping - respond with pong
    if (opcode == 0x09) {
        uint8_t pong[2] = {0x8A, 0x80}; // FIN + Pong, masked, 0 length
        uint8_t pmask[4];
        randomMask(pmask);
        uint8_t pongFrame[6] = {0x8A, 0x80, pmask[0], pmask[1], pmask[2], pmask[3]};
        sendAll(sock_, pongFrame, 6);
        out.clear();
        return true; // Return empty, caller should loop
    }

    // Text or continuation frame
    if (opcode == 0x01 || opcode == 0x00) {
        out.assign(payload.begin(), payload.end());
    }

    return true;
}

void WsSignaling::parseAndDispatch(const std::string &json) {
    if (!eventCb_) return;

    WsEvent ev = {};
    ev.type = jsonGetString(json, "type");
    ev.client_id = jsonGetString(json, "client_id");
    ev.channel_id = jsonGetString(json, "channel_id");
    ev.user_name = jsonGetString(json, "user_name");
    ev.message = jsonGetString(json, "message");
    ev.udp_port = (uint16_t)jsonGetUint64(json, "udp_port");
    ev.timestamp = jsonGetUint64(json, "timestamp");
    ev.server_time = jsonGetUint64(json, "server_time");

    eventCb_(ev);
}

void WsSignaling::recvLoop() {
    while (connected_.load()) {
        std::string msg;
        if (!recvWsMessage(msg)) {
            connected_.store(false);
            if (eventCb_) {
                WsEvent ev = {};
                ev.type = "disconnected";
                eventCb_(ev);
            }
            break;
        }
        if (!msg.empty()) {
            parseAndDispatch(msg);
        }
    }
}

uint64_t WsSignaling::deriveUdpId(const std::string &uuid_str) {
    // Parse UUID hex bytes (skip dashes): first 8 bytes -> u64 big-endian
    // UUID format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    uint8_t bytes[16] = {};
    int bi = 0;
    for (size_t i = 0; i < uuid_str.size() && bi < 16; i++) {
        char c = uuid_str[i];
        if (c == '-') continue;
        uint8_t nibble;
        if (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = 10 + c - 'a';
        else if (c >= 'A' && c <= 'F') nibble = 10 + c - 'A';
        else continue;

        if (bi % 2 == 0) {
            // Accumulate into even index: high nibble first
            // We process 2 hex chars per byte
        }
        // Simpler: collect all nibbles, pair them
        int byteIdx = bi / 2;
        if (bi % 2 == 0)
            bytes[byteIdx] = nibble << 4;
        else
            bytes[byteIdx] |= nibble;
        bi++;
    }

    // First 8 bytes as u64 big-endian (same as server's derive_udp_id)
    uint64_t id = 0;
    for (int i = 0; i < 8; i++)
        id = (id << 8) | bytes[i];
    return id;
}

bool WsSignaling::connect(const char *host, uint16_t port, const char *token) {
    if (connected_.load()) disconnect();

    // Initialize Winsock
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    if (!doHandshake(host, port)) {
        fprintf(stderr, "WS: Failed to connect to %s:%u\n", host, port);
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
        return false;
    }

    connected_.store(true);

    // Send auth message
    char authMsg[256];
    snprintf(authMsg, sizeof(authMsg),
             "{\"type\":\"auth\",\"token\":\"%s\"}", token);
    if (!sendWsText(authMsg)) {
        disconnect();
        return false;
    }

    // Wait for auth_ok synchronously (before starting recv thread)
    std::string response;
    if (!recvWsMessage(response)) {
        disconnect();
        return false;
    }

    std::string type = jsonGetString(response, "type");
    if (type != "auth_ok") {
        fprintf(stderr, "WS: Auth failed: %s\n", response.c_str());
        disconnect();
        return false;
    }

    clientId_ = jsonGetString(response, "client_id");
    udpPort_ = (uint16_t)jsonGetUint64(response, "udp_port");
    udpClientId_ = deriveUdpId(clientId_);

    // Start receive thread
    recvThread_ = std::thread(&WsSignaling::recvLoop, this);

    return true;
}

void WsSignaling::disconnect() {
    connected_.store(false);
    if (sock_ != INVALID_SOCKET) {
        // Send close frame
        uint8_t closeFrame[6] = {0x88, 0x80, 0, 0, 0, 0};
        send(sock_, (const char *)closeFrame, 6, 0);
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    if (recvThread_.joinable()) {
        recvThread_.join();
    }
    clientId_.clear();
    udpClientId_ = 0;
}

bool WsSignaling::joinChannel(const char *channel_id) {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"join_channel\",\"channel_id\":\"%s\"}", channel_id);
    return sendWsText(msg);
}

bool WsSignaling::leaveChannel(const char *channel_id) {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"leave_channel\",\"channel_id\":\"%s\"}", channel_id);
    return sendWsText(msg);
}

bool WsSignaling::startTransmit(const char *channel_id) {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"start_transmit\",\"channel_id\":\"%s\"}", channel_id);
    return sendWsText(msg);
}

bool WsSignaling::stopTransmit(const char *channel_id) {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"stop_transmit\",\"channel_id\":\"%s\"}", channel_id);
    return sendWsText(msg);
}

bool WsSignaling::sendPing(uint64_t timestamp) {
    char msg[128];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"ping\",\"timestamp\":%llu}", (unsigned long long)timestamp);
    return sendWsText(msg);
}

#endif // _WIN32
