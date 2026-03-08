/**
 * @file network.c
 * @brief Cross-platform UDP networking implementation.
 *
 * POSIX sockets for Linux/macOS/iOS/Android, Winsock for Windows.
 */

#include "network.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  /* Windows */
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")

  static int g_wsa_initialized = 0;

  static int platform_init(void)
  {
      if (!g_wsa_initialized) {
          WSADATA wsa;
          if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
              return -1;
          g_wsa_initialized = 1;
      }
      return 0;
  }

  static void platform_cleanup(void)
  {
      if (g_wsa_initialized) {
          WSACleanup();
          g_wsa_initialized = 0;
      }
  }

  static int set_nonblocking(ae_socket_t sock)
  {
      u_long mode = 1;
      return ioctlsocket(sock, FIONBIO, &mode) == 0 ? 0 : -1;
  }

  static int get_last_error(void)
  {
      return WSAGetLastError();
  }

  static int would_block(void)
  {
      int err = WSAGetLastError();
      return (err == WSAEWOULDBLOCK);
  }

  static void close_socket(ae_socket_t sock)
  {
      closesocket(sock);
  }

#else
  /* POSIX (Linux, macOS, iOS, Android) */
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <time.h>

  static int platform_init(void) { return 0; }
  static void platform_cleanup(void) { (void)0; }

  static int set_nonblocking(ae_socket_t sock)
  {
      int flags = fcntl(sock, F_GETFL, 0);
      if (flags < 0) return -1;
      return fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
  }

  static int would_block(void)
  {
      return (errno == EAGAIN || errno == EWOULDBLOCK);
  }

  static void close_socket(ae_socket_t sock)
  {
      close(sock);
  }
#endif

/* ─── Time utility ────────────────────────────────────────────────── */

uint64_t ae_net_time_ms(void)
{
#ifdef _WIN32
    /* Use QueryPerformanceCounter for monotonic time */
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)(now.QuadPart * 1000 / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
#endif
}

/* ─── Network context ─────────────────────────────────────────────── */

ae_net_t *ae_net_create(void)
{
    if (platform_init() != 0)
        return NULL;

    ae_net_t *net = (ae_net_t *)calloc(1, sizeof(ae_net_t));
    if (!net)
        return NULL;

    net->sock = AE_INVALID_SOCKET;
    net->connected = false;

    return net;
}

void ae_net_destroy(ae_net_t *net)
{
    if (!net)
        return;

    if (net->sock != AE_INVALID_SOCKET)
        close_socket(net->sock);

    free(net);
}

int ae_net_connect(ae_net_t *net, const char *host, uint16_t port)
{
    if (!net || !host)
        return -1;

    /* Close existing socket if any */
    if (net->sock != AE_INVALID_SOCKET) {
        close_socket(net->sock);
        net->sock = AE_INVALID_SOCKET;
        net->connected = false;
    }

    /* Resolve host */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;    /* IPv4 for POC */
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return -1;

    /* Create UDP socket */
    net->sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (net->sock == AE_INVALID_SOCKET) {
        freeaddrinfo(res);
        return -1;
    }

    /* Set non-blocking */
    if (set_nonblocking(net->sock) != 0) {
        close_socket(net->sock);
        net->sock = AE_INVALID_SOCKET;
        freeaddrinfo(res);
        return -1;
    }

    /* Set socket buffer sizes for low-latency audio */
    int sndbuf = 65536;
    int rcvbuf = 65536;
#ifdef _WIN32
    setsockopt(net->sock, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf));
    setsockopt(net->sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
#else
    setsockopt(net->sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(net->sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
#endif

    /* "Connect" the UDP socket to set default destination */
    if (connect(net->sock, res->ai_addr, (int)res->ai_addrlen) != 0) {
        close_socket(net->sock);
        net->sock = AE_INVALID_SOCKET;
        freeaddrinfo(res);
        return -1;
    }

    /* Save server info */
    strncpy(net->server_host, host, sizeof(net->server_host) - 1);
    net->server_port = port;

    /* Save resolved address */
    if (res->ai_addrlen <= sizeof(net->addr_storage)) {
        memcpy(net->addr_storage, res->ai_addr, res->ai_addrlen);
        net->addr_len = (int)res->ai_addrlen;
    }

    freeaddrinfo(res);
    net->connected = true;
    net->last_keepalive_ms = ae_net_time_ms();

    return 0;
}

void ae_net_disconnect(ae_net_t *net)
{
    if (!net)
        return;

    if (net->sock != AE_INVALID_SOCKET) {
        close_socket(net->sock);
        net->sock = AE_INVALID_SOCKET;
    }
    net->connected = false;
}

int ae_net_send(ae_net_t *net, const uint8_t *data, int len)
{
    if (!net || !data || len <= 0 || net->sock == AE_INVALID_SOCKET)
        return -1;

#ifdef _WIN32
    int sent = send(net->sock, (const char *)data, len, 0);
#else
    ssize_t sent = send(net->sock, data, (size_t)len, 0);
#endif

    if (sent < 0)
        return -1;

    net->last_send_time_ms = ae_net_time_ms();
    return (int)sent;
}

int ae_net_recv(ae_net_t *net, uint8_t *buf, int buf_size)
{
    if (!net || !buf || buf_size <= 0 || net->sock == AE_INVALID_SOCKET)
        return -1;

#ifdef _WIN32
    int received = recv(net->sock, (char *)buf, buf_size, 0);
    if (received < 0) {
        if (would_block())
            return 0;  /* no data available */
        return -1;
    }
#else
    ssize_t received = recv(net->sock, buf, (size_t)buf_size, 0);
    if (received < 0) {
        if (would_block())
            return 0;
        return -1;
    }
#endif

    return (int)received;
}

bool ae_net_is_connected(const ae_net_t *net)
{
    return net && net->connected;
}
