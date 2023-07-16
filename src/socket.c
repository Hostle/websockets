#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#endif

#if defined(__windows__)
#define _WIN32_WINNT 0x0601  // Windows 7 or later
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <openssl/rand.h>

#include "socket.h"

//------------------------------------------------------------------------------
// Internal functions
//------------------------------------------------------------------------------

/** @brief Defines the various states of a WebSocket connection */
typedef enum
{
    /** The connection with the client is in the initial SSL handshake phase. */
    CNX_SSL_INIT = (1 << 3),

} socket_flags_t;

/**
 * @brief Connects to a host at a specific port and returns the connection
 *        status.
 *
 * @param host The host to connect to.
 * @param port The port to connect to.
 * @return The connection status, 0 if successful, an error code otherwise.
 *
 * @ingroup ConnectionFunctions
 */
static int connect_to_host(const char* host, const char* port);

/**
 * @brief  Sets a timeout on a socket read/write operations.
 *
 * @param fd The socket file descriptor.
 * @param sec The timeout value in seconds.
 * @return True if successful, false otherwise.
 *
 * @ingroup SocketFunctions
 */
static bool socket_set_timeout(int fd, int sec);

/**
 * @brief Sets a socket to non-blocking mode.
 *
 * @param sockfd The socket file descriptor.
 * @return True if successful, false otherwise.
 *
 * @ingroup SocketFunctions
 */
static bool socket_set_nonblocking(int sockfd);

//------------------------------------------------------------------------------
//> Socket API
//------------------------------------------------------------------------------

vws_socket* vws_socket_new()
{
    vws_socket* c = (vws_socket*)vrtql.malloc(sizeof(vws_socket));
    memset(c, 0, sizeof(vws_socket));

    return vws_socket_ctor(c);
}

vws_socket* vws_socket_ctor(vws_socket* s)
{
    s->buffer  = vrtql_buffer_new();
    s->ssl     = NULL;
    s->ssl_ctx = NULL;
    s->timeout = 10000;
    s->trace   = 0;
    s->data    = NULL;
    s->hs      = NULL;

    return s;
}

void vws_socket_free(vws_socket* c)
{
    if (c == NULL)
    {
        return;
    }

    vws_socket_dtor(c);
}

void vws_socket_dtor(vws_socket* s)
{
    vws_socket_disconnect(s);

    // Free receive buffer
    vrtql_buffer_free(s->buffer);

    if (s->sockfd >= 0)
    {
        close(s->sockfd);
    }

    // Free connection
    free(s);
}

//------------------------------------------------------------------------------
// Utility functions
//------------------------------------------------------------------------------

bool vws_socket_set_timeout(vws_socket* s, int sec)
{
    return socket_set_timeout(s->sockfd, sec);
}

bool socket_set_timeout(int fd, int sec)
{
#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)

    if (fd < 0)
    {
        vrtql.error(VE_RT, "Not connected");
        return false;
    }

    struct timeval tm;
    tm.tv_sec  = sec;
    tm.tv_usec = 0;

    // Set the send timeout
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&tm, sizeof(tm)) < 0)
    {
        vrtql.error(VE_SYS, "setsockopt failed");

        return false;
    }

    // Set the receive timeout
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tm, sizeof(tm)) < 0)
    {
        vrtql.error(VE_SYS, "setsockopt failed");

        return false;
    }

#elif defined(__windows__)

    if (fd == INVALID_SOCKET)
    {
        vrtql.error(VE_RT, "Not connected");
        return false;
    }

    // Convert from sec to ms for Windows
    DWORD tm = sec * 1000;

    // Set the send timeout
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (cstr)&tm, sizeof(tm)) < 0)
    {
        vrtql.error(VE_SYS, "setsockopt failed");

        return false;
    }

    // Set the receive timeout
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (cstr)&tm, sizeof(tm)) < 0)
    {
        vrtql.error(VE_SYS, "setsockopt failed");

        return false;
    }

#else
    #error Platform not supported
#endif

    vrtql.success();

    return true;
}

bool socket_set_nonblocking(int sockfd)
{
#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)

    int flags = fcntl(sockfd, F_GETFL, 0);

    if (flags == -1)
    {
        vrtql.error(VE_SYS, "fcntl(sockfd, F_GETFL, 0) failed");

        return false;
    }

    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        vrtql.error(VE_SYS, "fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) failed");

        return false;
    }

#elif defined(__windows__)

    unsigned long arg = 1;
    if (ioctlsocket(sockfd, FIONBIO, &arg) == SOCKET_ERROR)
    {
        vrtql.error(VE_SYS, "ioctlsocket(sockfd, FIONBIO, &arg)");

        return false;
    }

#else
    #error Platform not supported
#endif

    vrtql.success();

    return true;
}

bool vws_socket_is_connected(vws_socket* c)
{
    if (c == NULL)
    {
        return false;
    }

    return c->sockfd > 0;
}

bool vws_socket_connect(vws_socket* c, cstr host, int port, bool ssl)
{
    if (c == NULL)
    {
        // Return early if failed to create a connection.
        vrtql.error(VE_RT, "Invalid connection pointer()");
        return false;
    }

    if (ssl == true)
    {
        if (vrtql_is_flag(&vrtql.state, CNX_SSL_INIT) == false)
        {
            SSL_library_init();
            RAND_poll();
            SSL_load_error_strings();

            c->ssl_ctx = SSL_CTX_new(TLS_method());

            if (c->ssl_ctx == NULL)
            {
                vrtql.error(VE_SYS, "Failed to create new SSL context");
                return false;
            }

            SSL_CTX_set_options(c->ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

            c->ssl = SSL_new(c->ssl_ctx);

            if (c->ssl == NULL)
            {
                vrtql.error(VE_SYS, "Failed to create new SSL object");
                vws_socket_close(c);
                return false;
            }

            vrtql_set_flag(&vrtql.state, CNX_SSL_INIT);
        }
    }

    char port_str[20];
    sprintf(port_str, "%d", port);
    c->sockfd = connect_to_host(host, port_str);

    if (c->sockfd < 0)
    {
        vrtql.error(VE_SYS, "Connection failed");
        vws_socket_close(c);
        return false;
    }

    // Set default timeout

    if (socket_set_timeout(c->sockfd, c->timeout/1000) == false)
    {
        // Error already set
        vws_socket_close(c);
        return false;
    }

    if (c->ssl != NULL)
    {
        SSL_set_fd(c->ssl, c->sockfd);

        if (SSL_connect(c->ssl) <= 0)
        {
            vrtql.error(VE_SYS, "SSL connection failed");
            vws_socket_close(c);
            return false;
        }
    }

    // Check if handshake handler is registered
    if (c->hs != NULL)
    {
        if (c->hs(c) == false)
        {
            vrtql.error(VE_SYS, "Handshake failed");
            vws_socket_close(c);
            return false;
        }
    }

    // Go into non-blocking mode as we are using poll() for socket_read() and
    // socket_write().
    if (socket_set_nonblocking(c->sockfd) == false)
    {
        // Error already set
        vws_socket_close(c);
        return false;
    }

    vrtql.success();

    return true;
}

void vws_socket_disconnect(vws_socket* c)
{
    if (vws_socket_is_connected(c) == false)
    {
        return;
    }

    vws_socket_close(c);

    vrtql.success();
}

ssize_t vws_socket_read(vws_socket* c)
{
    // Default success unless error
    vrtql.success();

    // Validate input parameters
    if (c == NULL)
    {
        vrtql.error(VE_WARN, "Invalid parameters");
        return -1;
    }

#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)

    struct pollfd fds;
    fds.fd     = c->sockfd;
    fds.events = POLLIN;

    int poll_result = poll(&fds, 1, c->timeout);

#elif defined(__windows__)

    WSAPOLLFD fds;
    fds.fd     = c->sockfd;
    fds.events = POLLIN;

    int poll_result = WSAPoll(&fds, 1, c->timeout);

#else
    #error Platform not supported
#endif

    if (poll_result == -1)
    {
        vrtql.error(VE_RT, "poll() failed");
        return -1;
    }

    if (poll_result == 0)
    {
        vrtql.error(VE_WARN, "timeout");
        return 0;
    }

    unsigned char data[1024];
    int size = 1024;
    ssize_t n;

    if (fds.revents & POLLIN)
    {
        if (c->ssl != NULL)
        {
            // SSL socket is readable, perform SSL_read() operation
            n = SSL_read(c->ssl, data, size);
        }
        else
        {
            // Non-SSL socket is readable, perform recv() operation
            n = recv(c->sockfd, data, size, 0);

            if (n == -1)
            {
                vrtql.error(VE_WARN, "recv() failed");
            }
        }
    }

    if (n > 0)
    {
        vrtql_buffer_append(c->buffer, data, n);
    }
    else
    {
        vrtql.error(VE_WARN, "Unexpected event in poll()");
    }

    return n;
}

ssize_t vws_socket_write(vws_socket* c, const ucstr data, size_t size)
{
    // Default success unless error
    vrtql.success();

    if (vws_socket_is_connected(c) == false)
    {
        vrtql.error(VE_RT, "Not connected");
        return -1;
    }

    // Validate input parameters
    if (c == NULL || data == NULL || size == 0)
    {
        vrtql.error(VE_WARN, "Invalid parameters");
        return -1;
    }

#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)

    struct pollfd fds;
    fds.fd     = c->sockfd;
    fds.events = POLLOUT;

    int poll_result = poll(&fds, 1, c->timeout);

#elif defined(__windows__)

    WSAPOLLFD fds;
    fds.fd     = c->sockfd;
    fds.events = POLLOUT;

    int poll_result = WSAPoll(&fds, 1, c->timeout);

#else
    #error Platform not supported
#endif

    if (poll_result == -1)
    {
        vrtql.error(VE_SYS, "poll() failed");
        return -1;
    }

    if (poll_result == 0)
    {
        vrtql.error(VE_TIMEOUT, "timeout");
        return 0;
    }

    if (fds.revents & POLLOUT)
    {
        if (c->ssl != NULL)
        {
            // SSL socket is writable, perform SSL_write() operation
            return SSL_write(c->ssl, data, size);
        }
        else
        {
            // Non-SSL socket is writable, perform send() operation
            ssize_t bytes_sent = send(c->sockfd, data, size, 0);
            if (bytes_sent == -1)
            {
                vrtql.error(VE_SYS, "send() failed");
            }

            return bytes_sent;
        }
    }

    vrtql.error(VE_SYS, "Unexpected event in poll()");

    return -1;
}

void vws_socket_close(vws_socket* c)
{
    if (c->ssl != NULL)
    {
        int rc = SSL_shutdown(c->ssl);

        if (rc == 0)
        {
            // the shutdown is not yet finished
            rc = SSL_shutdown(c->ssl);
        }

        if (rc != 1)
        {
            vrtql.error(VE_WARN, "SSL_shutdown failed");
        }

        SSL_free(c->ssl);
        c->ssl = NULL;
    }

    if (c->ssl_ctx != NULL)
    {
        SSL_CTX_free(c->ssl_ctx);
        c->ssl_ctx = NULL;
    }

    if (c->sockfd > 0)
    {
#if defined(__windows__)
        if (closesocket(c->sockfd) == SOCKET_ERROR)
#else
        if (close(c->sockfd) == -1)
#endif
        {
            vrtql.error(VE_SYS, "Socket close failed");
        }

#if defined(__windows__)
        WSACleanup();
#endif

        c->sockfd = -1;
    }
}

int connect_to_host(const char* host, const char* port)
{
    int sockfd = -1;

#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)

    // Resolve the host
    struct addrinfo hints, *res, *res0;
    int error;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = PF_UNSPEC; // Accept any family (IPv4 or IPv6)
    hints.ai_socktype = SOCK_STREAM;

    error = getaddrinfo(host, port, &hints, &res0);

    if (error)
    {
        if (vrtql.trace)
        {
            cstr msg = gai_strerror(error);
            vrtql_trace(VL_ERROR, "getaddrinfo failed: %s: %s", host, msg);
        }

        vrtql.error(VE_SYS, "getaddrinfo() failed");

        return -1;
    }

    for (res = res0; res; res = res->ai_next)
    {
        sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

        if (sockfd == -1)
        {
            vrtql.error(VE_SYS, "Failed to create socket");
            continue;
        }

        if (connect(sockfd, res->ai_addr, res->ai_addrlen) == -1)
        {
            close(sockfd);
            sockfd = -1;

            vrtql.error(VE_SYS, "Failed to connect");
            continue;
        }

        break; // If we get here, we must have connected successfully
    }

    freeaddrinfo(res0); // Free the addrinfo structure for this host

#elif defined(__windows__)

    // Windows specific implementation
    // Please refer to Windows Socket programming guide

    WSADATA wsaData;
    struct addrinfo *result = NULL, *ptr = NULL, hints;
    sockfd = INVALID_SOCKET;

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0)
    {
        vrtql.error(VE_SYS, "WSAStartup failed");
        return -1;
    }

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    // Resolve the server address and port
    if (getaddrinfo(host, port, &hints, &result) != 0)
    {
        vrtql.error(VE_SYS, "getaddrinfo failed\n");
        return -1;
    }

    // Attempt to connect to an address until one succeeds
    for (ptr = result; ptr != NULL; ptr =ptr->ai_next)
    {
        // Create a SOCKET for connecting to server
        sockfd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);

        if (sockfd == INVALID_SOCKET)
        {
            char buf[256];
            int e = WSAGetLastError();
            snprintf(buf, sizeof(buf), "socket failed with error: %ld", e);
            vrtql.error(VE_RT, buf);

            WSACleanup();
            return -1;
        }

        // Connect to server.
        if (connect(sockfd, ptr->ai_addr, (int)ptr->ai_addrlen) == SOCKET_ERROR)
        {
            closesocket(sockfd);
            sockfd = INVALID_SOCKET;
            continue;
        }

        break;
    }

    freeaddrinfo(result);

    if (sockfd == INVALID_SOCKET)
    {
        vrtql.error(VE_SYS, "Unable to connect to host");
        WSACleanup();
        return -1;
    }

#else
    #error Platform not supported
#endif

    vrtql.success();

    return sockfd;
}
