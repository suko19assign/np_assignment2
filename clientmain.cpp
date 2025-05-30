#include <iostream>
#include <iomanip>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <chrono>
#include <thread>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "protocol.h"     
#include <calcLib.h>     


// Helpers                                                               


#ifdef DEBUG
#   define DBG(x) do{ x; }while(0)
#else
#   define DBG(x) do{}while(0)
#endif

static constexpr int   MAX_TRIES         = 3;          // 1 original + 2 retransmissions
static constexpr auto  WAIT              = std::chrono::seconds{2};
static constexpr uint8_t SUPP_MAJ_VER    = 1;
static constexpr uint8_t SUPP_MIN_VER    = 0;

/* Convert the textual <host:port> argument to a sockaddr (IPv4 / IPv6) */
static int resolveDest(const std::string& hp, sockaddr_storage& out, socklen_t& len)
{
    auto colon = hp.rfind(':');
    if (colon == std::string::npos)
        return -1;

    std::string host = hp.substr(0, colon);
    std::string port = hp.substr(colon + 1);

    addrinfo  hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    int rv = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (rv) {
        std::cerr << "getaddrinfo: " << gai_strerror(rv) << '\n';
        return -1;
    }
    /* Pick the first address that works. */
    memcpy(&out, res->ai_addr, res->ai_addrlen);
    len = res->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

/* Pretty-print an address. */
static std::string addrToString(const sockaddr_storage& s)
{
    char hbuf[NI_MAXHOST]{}, pbuf[NI_MAXSERV]{};
    getnameinfo(reinterpret_cast<const sockaddr*>(&s), sizeof(s),
                hbuf, sizeof(hbuf), pbuf, sizeof(pbuf),
                NI_NUMERICHOST | NI_NUMERICSERV);
    return std::string{hbuf} + ":" + pbuf;
}

/* Send a buffer and wait (with timeout) for the next datagram. */
static ssize_t txRxWithRetry(int sock,
                             const void* sndBuf, size_t sndLen,
                             void*       rcvBuf, size_t rcvLen)
{
    for (int attempt = 1; attempt <= MAX_TRIES; ++attempt)
    {
        if (send(sock, sndBuf, sndLen, 0) != static_cast<ssize_t>(sndLen))
            perror("send");

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);

        timeval tv{};
        tv.tv_sec  = WAIT.count();

        int rv = select(sock + 1, &rfds, nullptr, nullptr, &tv);
        if (rv > 0)
        {
            ssize_t got = recv(sock, rcvBuf, rcvLen, 0);
            if (got >= 0) return got;
        }
        /* timed out – try again */
        DBG(std::cerr << "Timeout #" << attempt << ", retransmitting …\n");
    }
    return -1;      // exhausted all retries
}
