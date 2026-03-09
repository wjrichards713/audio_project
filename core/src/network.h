/**
 * @file network.h
 * @brief Cross-platform UDP networking abstraction.
 *
 * Supports POSIX sockets (Linux, macOS, iOS, Android) and Winsock (Windows).
 * Non-blocking I/O for use from the audio processing path.
 */

#ifndef NETWORK_H
#define NETWORK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Platform socket abstraction ─────────────────────────────────── */

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET ae_socket_t;
  #define AE_INVALID_SOCKET INVALID_SOCKET
#else
  typedef int ae_socket_t;
  #define AE_INVALID_SOCKET (-1)
#endif

/* ─── Network context ─────────────────────────────────────────────── */

typedef struct {
    ae_socket_t   sock;
    bool          connected;
    uint64_t      last_send_time_ms;
    uint64_t      last_keepalive_ms;

    /* Server address (set by ae_net_connect) */
    char          server_host[256];
    uint16_t      server_port;

    /* Platform-specific address storage */
    uint8_t       addr_storage[128];  /* sockaddr_storage equivalent */
    int           addr_len;
} ae_net_t;

/**
 * Create a UDP socket context.
 * @return Network context, or NULL on failure.
 */
ae_net_t *ae_net_create(void);

/** Destroy the network context and close the socket. */
void ae_net_destroy(ae_net_t *net);

/**
 * Set the server address and "connect" the UDP socket.
 * (UDP connect sets the default destination for send().)
 *
 * @param net   Network context.
 * @param host  Server hostname or IP address.
 * @param port  Server UDP port.
 * @return 0 on success, -1 on failure.
 */
int ae_net_connect(ae_net_t *net, const char *host, uint16_t port);

/**
 * Disconnect (close and reopen the socket).
 */
void ae_net_disconnect(ae_net_t *net);

/**
 * Send a packet to the connected server.
 * @param net   Network context.
 * @param data  Packet data.
 * @param len   Packet length.
 * @return Number of bytes sent, or -1 on error.
 */
int ae_net_send(ae_net_t *net, const uint8_t *data, int len);

/**
 * Non-blocking receive.
 * @param net       Network context.
 * @param buf       Receive buffer.
 * @param buf_size  Size of receive buffer.
 * @return Number of bytes received, 0 if no data available, -1 on error.
 */
int ae_net_recv(ae_net_t *net, uint8_t *buf, int buf_size);

/**
 * Check if the socket is connected.
 */
bool ae_net_is_connected(const ae_net_t *net);

/**
 * Get current time in milliseconds (monotonic clock).
 * Cross-platform utility used for keepalive timing.
 */
uint64_t ae_net_time_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_H */
