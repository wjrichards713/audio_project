/**
 * ws_signaling.h - Minimal WebSocket signaling client for audio server
 *
 * Handles the JSON signaling protocol over WebSocket:
 * auth, join/leave channels, start/stop transmit, ping.
 */

#ifndef WS_SIGNALING_H
#define WS_SIGNALING_H

#ifdef _WIN32

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

struct WsEvent {
    std::string type;
    std::string client_id;
    std::string channel_id;
    std::string user_name;
    std::string message;
    uint16_t udp_port;
    uint64_t timestamp;
    uint64_t server_time;
};

using WsEventCallback = std::function<void(const WsEvent &)>;

class WsSignaling {
public:
    WsSignaling();
    ~WsSignaling();

    /** Connect to server and authenticate.
     *  Returns true if WebSocket connected and auth_ok received. */
    bool connect(const char *host, uint16_t port, const char *token);

    /** Disconnect and close the socket. */
    void disconnect();

    bool isConnected() const { return connected_.load(); }

    /** Get the UUID client_id string from auth_ok. */
    std::string getClientId() const { return clientId_; }

    /** Get the numeric UDP client_id (derived from UUID, same as server). */
    uint64_t getUdpClientId() const { return udpClientId_; }

    /** Get the UDP port from auth_ok. */
    uint16_t getUdpPort() const { return udpPort_; }

    /* Signaling commands */
    bool joinChannel(const char *channel_id);
    bool leaveChannel(const char *channel_id);
    bool startTransmit(const char *channel_id);
    bool stopTransmit(const char *channel_id);
    bool sendPing(uint64_t timestamp);

    /** Set callback for incoming server events. */
    void setEventCallback(WsEventCallback cb) { eventCb_ = cb; }

private:
    SOCKET sock_ = INVALID_SOCKET;
    std::atomic<bool> connected_{false};
    std::thread recvThread_;
    WsEventCallback eventCb_;
    std::mutex sendMutex_;

    std::string clientId_;
    uint64_t udpClientId_ = 0;
    uint16_t udpPort_ = 0;

    bool doHandshake(const char *host, uint16_t port);
    bool sendWsText(const std::string &text);
    bool recvWsMessage(std::string &out);
    void recvLoop();
    void parseAndDispatch(const std::string &json);

    static uint64_t deriveUdpId(const std::string &uuid_str);
};

#endif // _WIN32
#endif // WS_SIGNALING_H
