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

//app

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <host:port>\n";
        return 1;
    }

    /* resolve dest */
    sockaddr_storage dest{};
    socklen_t        destLen{};
    if (resolveDest(argv[1], dest, destLen) != 0)
        return 1;

    std::cout << "Host " << argv[1] << ".\n";

    /* open socket */
    int sock = socket(dest.ss_family, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    if (connect(sock,
                reinterpret_cast<sockaddr*>(&dest), destLen) != 0)
    { perror("connect"); return 1; }

#ifdef DEBUG
    /* Show the locally bound address */
    sockaddr_storage local{};
    socklen_t lLen = sizeof(local);
    getsockname(sock, reinterpret_cast<sockaddr*>(&local), &lLen);
    std::cerr << "Connected to " << addrToString(dest)
              << " local "        << addrToString(local) << '\n';
#endif

    /* handshake: HELLO */
    calcMessage hello{};
    hello.type          = htons(22);
    hello.message       = htonl(0);
    hello.protocol      = htons(17);        // UDP
    hello.major_version = htons(SUPP_MAJ_VER);
    hello.minor_version = htons(SUPP_MIN_VER);

    std::aligned_storage_t<sizeof(calcProtocol) ,alignof(calcProtocol)> rxBuf; // big enough
    ssize_t got = txRxWithRetry(sock,
                                &hello, sizeof(hello),
                                &rxBuf, sizeof(rxBuf));

    if (got < 0) {
        std::cerr << "ERROR: server did not answer within 6 s, giving up.\n";
        return 1;
    }

    /*  decode reply  */
    if (static_cast<size_t>(got) == sizeof(calcMessage)) {
        auto* cm = reinterpret_cast<calcMessage*>(&rxBuf);
        if (ntohs(cm->message) == 2) {
            std::cerr << "Server replied NOT OK – protocol version rejected.\n";
            return 1;
        }
        std::cerr << "ERROR WRONG SIZE OR INCORRECT PROTOCOL\n";
        return 1;
    }
    if (static_cast<size_t>(got) != sizeof(calcProtocol)) {
        std::cerr << "ERROR WRONG SIZE OR INCORRECT PROTOCOL\n";
        return 1;
    }

    auto* task = reinterpret_cast<calcProtocol*>(&rxBuf);

    /* host-endian copies                                            */
    uint16_t srvType  = ntohs(task->type);
    uint32_t id       = ntohl(task->id);
    uint32_t arith    = ntohl(task->arith);
    int32_t  a        = ntohl(task->inValue1);
    int32_t  b        = ntohl(task->inValue2);
    double   fa       = task->flValue1;
    double   fb       = task->flValue2;

    /*  print task */
    const char* opStr = nullptr;
    double      fRes  = 0.0;
    int32_t     iRes  = 0;

    switch (arith)
    {
        case 1: opStr="add";  iRes = a + b;               break;
        case 2: opStr="sub";  iRes = a - b;               break;
        case 3: opStr="mul";  iRes = a * b;               break;
        case 4: opStr="div";  iRes = a / b;               break;
        case 5: opStr="fadd"; fRes = fa + fb;             break;
        case 6: opStr="fsub"; fRes = fa - fb;             break;
        case 7: opStr="fmul"; fRes = fa * fb;             break;
        case 8: opStr="fdiv"; fRes = fa / fb;             break;
        default:
            std::cerr << "Unknown operator from server.\n";
            return 1;
    }

    std::cout << "ASSIGNMENT: " << opStr << ' '
              << ((arith <= 4) ? std::to_string(a) : std::to_string(fa)) << ' '
              << ((arith <= 4) ? std::to_string(b) : std::to_string(fb)) << '\n';

    DBG(if (arith <= 4)
        std::cerr << "Calculated the result to " << iRes << '\n';
        else
        std::cerr << "Calculated the result to "
                  << std::setprecision(8) << fRes << '\n';)

    /* send result  */
    calcProtocol reply{};
    reply.type          = htons(2);     // client to server
    reply.major_version = htons(SUPP_MAJ_VER);
    reply.minor_version = htons(SUPP_MIN_VER);
    reply.id            = htonl(id);
    reply.arith         = htonl(arith);
    reply.inValue1      = htonl(a);
    reply.inValue2      = htonl(b);
    reply.inResult      = htonl(iRes);
    reply.flValue1      = fa;
    reply.flValue2      = fb;
    reply.flResult      = fRes;

    calcMessage verdict{};
    got = txRxWithRetry(sock,
                        &reply, sizeof(reply),
                        &verdict, sizeof(verdict));
    if (got < 0) {
        std::cerr << "ERROR: server did not confirm within 6 s, giving up.\n";
        return 1;
    }
    if (static_cast<size_t>(got) != sizeof(calcMessage)) {
        std::cerr << "ERROR WRONG SIZE OR INCORRECT PROTOCOL\n";
        return 1;
    }

    uint32_t m = ntohl(verdict.message);
    std::cout << (m == 1 ? "OK" : "ERROR")
              << " (myresult="
              << (arith <= 4 ? std::to_string(iRes)
                             : std::to_string(fRes))
              << ")\n";

    return 0;
}
