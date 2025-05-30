#include <iostream>
#include <iomanip>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <unordered_map>
#include <chrono>
#include <cmath>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "protocol.h"
#include <calcLib.h>

#ifdef DEBUG
#define DBG(x) do { x; } while (0)
#else
#define DBG(x) do {} while (0)
#endif

static constexpr uint8_t SUPP_MAJ_VER = 1;
static constexpr uint8_t SUPP_MIN_VER = 0;
static constexpr int     JOB_TTL_S    = 10;

//map and key type
struct AddrKey {
    sockaddr_storage s{};
    socklen_t        len{};
    bool operator==(const AddrKey& o) const {
        return len == o.len && std::memcmp(&s, &o.s, len) == 0;
    }
};
struct AddrKeyHash {
    std::size_t operator()(const AddrKey& k) const noexcept
    {
        const std::uint64_t* p =
            reinterpret_cast<const std::uint64_t*>(&k.s);
        std::size_t h = 0;
        for (std::size_t i = 0; i < sizeof(k.s) / sizeof(std::uint64_t); ++i)
            h ^= p[i] + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

struct Job {
    uint32_t id{};
    uint32_t arith{};
    int32_t  ia{}, ib{}, ires{};
    double   fa{}, fb{}, fres{};
    std::chrono::steady_clock::time_point deadline{};
};

using JobMap = std::unordered_map<AddrKey, Job, AddrKeyHash>;


static std::string addrToString(const sockaddr_storage& s, socklen_t len)
{
    char hbuf[NI_MAXHOST]{}, pbuf[NI_MAXSERV]{};
    getnameinfo(reinterpret_cast<const sockaddr*>(&s), len,
                hbuf, sizeof(hbuf), pbuf, sizeof(pbuf),
                NI_NUMERICHOST | NI_NUMERICSERV);
    return std::string(hbuf) + ":" + pbuf;
}

//main

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <bind-host:port>\n";
        return 1;
    }

    /* bind address  */

    std::string hp{argv[1]};
    auto colon = hp.rfind(':');
    if (colon == std::string::npos) {
        std::cerr << "Invalid address format.\n";
        return 1;
    }
    std::string host = hp.substr(0, colon);
    std::string port = hp.substr(colon + 1);

    addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags    = AI_PASSIVE;

    if (int rv = getaddrinfo(host.c_str(), port.c_str(), &hints, &res); rv) {
        std::cerr << "getaddrinfo: " << gai_strerror(rv) << '\n';
        return 1;
    }

    int sock = -1;
    for (addrinfo* p = res; p; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, 0);
        if (sock < 0) continue;
        if (bind(sock, p->ai_addr, p->ai_addrlen) == 0) {
            std::cout << "Listening on "
                      << addrToString(
                             *reinterpret_cast<sockaddr_storage*>(p->ai_addr),
                             p->ai_addrlen)
                      << '\n';
            break;
        }
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    if (sock < 0) { perror("bind"); return 1; }

    /* init and loop  */

    initCalcLib();
    uint32_t nextId = 1;
    JobMap   jobs;

    for (;;) {
        sockaddr_storage from{};
        socklen_t        fromLen = sizeof(from);

        std::aligned_storage_t<
            (sizeof(calcProtocol) > sizeof(calcMessage)
                 ? sizeof(calcProtocol)
                 : sizeof(calcMessage)),
            alignof(calcProtocol)>
            buf;

        ssize_t got = recvfrom(sock, &buf, sizeof(buf), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (got < 0) { perror("recvfrom"); continue; }

        std::cout << "RX " << got << " B from "
                  << addrToString(from, fromLen) << '\n';

        /* HELLO from client  */

        if (static_cast<size_t>(got) == sizeof(calcMessage)) {
            auto* cm = reinterpret_cast<calcMessage*>(&buf);

            bool versionOK =
                ntohs(cm->type) == 22 &&
                ntohl(cm->message) == 0 &&
                ntohs(cm->major_version) == SUPP_MAJ_VER &&
                ntohs(cm->minor_version) == SUPP_MIN_VER;

            if (!versionOK) {
                calcMessage rej{};
                rej.type          = htons(2);
                rej.message       = htonl(2);
                rej.protocol      = htons(17);
                rej.major_version = htons(SUPP_MAJ_VER);
                rej.minor_version = htons(SUPP_MIN_VER);

                sendto(sock, &rej, sizeof(rej), 0,
                       reinterpret_cast<sockaddr*>(&from), fromLen);
                continue;
            }

            /* build new assignment */
            char* op = randomType();
            bool  fp = (op[0] == 'f');

            Job job;
            job.id    = nextId++;
            job.arith = fp
                        ? (strcmp(op, "fadd") == 0 ? 5
                           : strcmp(op, "fsub") == 0 ? 6
                           : strcmp(op, "fmul") == 0 ? 7
                                                     : 8)
                        : (strcmp(op, "add") == 0 ? 1
                           : strcmp(op, "sub") == 0 ? 2
                           : strcmp(op, "mul") == 0 ? 3
                                                    : 4);

            if (fp) {
                job.fa = randomFloat();
                job.fb = randomFloat();
                switch (job.arith) {
                    case 5: job.fres = job.fa + job.fb; break;
                    case 6: job.fres = job.fa - job.fb; break;
                    case 7: job.fres = job.fa * job.fb; break;
                    case 8: job.fres = job.fa / job.fb; break;
                }
            } else {
                job.ia = randomInt();
                job.ib = randomInt();
                switch (job.arith) {
                    case 1: job.ires = job.ia + job.ib; break;
                    case 2: job.ires = job.ia - job.ib; break;
                    case 3: job.ires = job.ia * job.ib; break;
                    case 4: job.ires = job.ia / job.ib; break;
                }
            }
            job.deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(JOB_TTL_S);

            AddrKey k{from, fromLen};
            jobs[k] = job;

            calcProtocol tp{};
            tp.type          = htons(1);
            tp.major_version = htons(SUPP_MAJ_VER);
            tp.minor_version = htons(SUPP_MIN_VER);
            tp.id            = htonl(job.id);
            tp.arith         = htonl(job.arith);
            tp.inValue1      = htonl(job.ia);
            tp.inValue2      = htonl(job.ib);
            tp.flValue1      = job.fa;
            tp.flValue2      = job.fb;

            sendto(sock, &tp, sizeof(tp), 0,
                   reinterpret_cast<sockaddr*>(&from), fromLen);
            continue;
        }

        /* RESULT from client  */

        if (static_cast<size_t>(got) == sizeof(calcProtocol)) {
            auto* cp = reinterpret_cast<calcProtocol*>(&buf);
            if (ntohs(cp->type) != 2) continue;   /* not a result */

            AddrKey k{from, fromLen};
            auto    it = jobs.find(k);
            bool    ok = false;

            if (it != jobs.end() && it->second.id == ntohl(cp->id)) {
                Job& job = it->second;

                if (job.arith <= 4) {
                    ok = static_cast<uint32_t>(job.ires) == ntohl(cp->inResult);
                } else {
                    double diff = std::fabs(job.fres - cp->flResult);
                    ok = diff < 1e-4;
                }
                jobs.erase(it);
            }

            calcMessage v{};
            v.type          = htons(2);
            v.message       = htonl(ok ? 1 : 2);
            v.protocol      = htons(17);
            v.major_version = htons(SUPP_MAJ_VER);
            v.minor_version = htons(SUPP_MIN_VER);

            sendto(sock, &v, sizeof(v), 0,
                   reinterpret_cast<sockaddr*>(&from), fromLen);
            continue;
        }

        /* sweep old jobs */

        auto now = std::chrono::steady_clock::now();
        for (auto it = jobs.begin(); it != jobs.end(); ) {
            if (it->second.deadline < now) {
                DBG(std::cerr << "Job for "
                              << addrToString(it->first.s, it->first.len)
                              << " expired.\n");
                it = jobs.erase(it);
            } else {
                ++it;
            }
        }
    }
}
