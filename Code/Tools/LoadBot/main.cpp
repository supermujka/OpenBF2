#include "LoadBotClient.h"
#include "NavMesh.h"
#include "RouteLoader.h"
#include "Telemetry.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <algorithm>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#endif

namespace {

#ifdef _WIN32
struct WinsockGuard {
    bool ok;
    WinsockGuard() : ok(false) {
        WSADATA wsa;
        ok = (::WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    }
    ~WinsockGuard() {
        if (ok) {
            ::WSACleanup();
        }
    }
};
#endif

volatile sig_atomic_t g_stopRequested = 0;

void handleSignal(int /*sig*/) {
    g_stopRequested = 1;
}

uint64_t steadyNowUs() {
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

struct RunnerOptions {
    std::string host;
    uint16_t port;
    uint32_t bots;
    double pingMs;
    double jitterMs;
    double lossPct;
    double pingMinMs;       // < 0: per-bot ping range disabled
    double pingMaxMs;
    double highPingPct;     // share of bots drawn from the high-ping range
    double highPingMinMs;
    double highPingMaxMs;
    int durationSec;
    std::string navmeshPath;
    std::string routesPath;
    std::string modName;
    std::string password;
    int staggerMs;
    float worldSize;
    bool worldSizeExplicit;
    uint32_t seed;
    int reportSec;
    bf2bot::AimConfig aim;
    int32_t forceTeam;  // 0 = alternate teams by bot index
    bool verbose;

    RunnerOptions()
        : host("127.0.0.1"),
          port(16567u),
          bots(64u),
          pingMs(35.0),
          jitterMs(8.0),
          lossPct(1.0),
          pingMinMs(-1.0),
          pingMaxMs(-1.0),
          highPingPct(0.0),
          highPingMinMs(50.0),
          highPingMaxMs(100.0),
          durationSec(300),
          navmeshPath(),
          routesPath(),
          modName("mods/bf2"),
          password(),
          staggerMs(150),
          worldSize(1024.0f),
          worldSizeExplicit(false),
          seed(1337u),
          reportSec(10),
          forceTeam(0),
          verbose(false) {}
};

void printUsage(const char* prog) {
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "OpenBF2 64-player standalone UDP load-test bot runner (strict BF2 v1.50 wire protocol)\n"
        "\n"
        "Options:\n"
        "  --host <ip|hostname>     Server address (default: 127.0.0.1)\n"
        "  --port <port>            Server UDP port (default: 16567)\n"
        "  --bots <1..64>           Number of concurrent bots (default: 64)\n"
        "  --ping-ms <float>        Simulated round-trip ping in ms, split 50/50 uplink/downlink (default: 35.0)\n"
        "  --jitter-ms <float>      Simulated RTT jitter in ms, split 50/50 per direction (default: 8.0)\n"
        "  --loss-pct <float>       Simulated packet loss %% applied independently per direction (default: 1.0)\n"
        "  --ping-min-ms <float>    With --ping-max-ms: each bot gets its own RTT drawn uniformly\n"
        "  --ping-max-ms <float>    from [min,max] (overrides --ping-ms; --jitter-ms still applies)\n"
        "  --high-ping-pct <float>  Percent of bots drawn from the high-ping range instead (default: 0)\n"
        "  --high-ping-min-ms <f>   High-ping range minimum RTT (default: 50)\n"
        "  --high-ping-max-ms <f>   High-ping range maximum RTT (default: 100)\n"
        "  --duration-sec <int>     Run duration in seconds; 0 = run until SIGINT/SIGTERM (default: 300)\n"
        "  --navmesh <dir_or_qtr>   Optional BF2 AIPathFinding v3 .qtr file or directory (Tier 1 nav)\n"
        "  --routes <routes.bin>    Optional OBR1 recorded route file (Tier 2 nav)\n"
        "  --mod <modName>          BF2 mod path sent in ConnectRequest (default: mods/bf2)\n"
        "  --password <pw>          Optional server password (default: empty)\n"
        "  --stagger-ms <int>       Delay between consecutive bot joins in ms (default: 150)\n"
        "  --world-size <float>     Map world size in meters (default: 1024.0; auto-set from navmesh if loaded)\n"
        "  --seed <uint32>          Deterministic RNG seed (default: 1337)\n"
        "  --aim-skill <0..1>       Human-like aim skill: scales reaction time, aim error, convergence (default: 0.7)\n"
        "  --interp-ms <int>        Client interpolation delay: bots aim at the position rendered at now-interp (default: 100)\n"
        "  --aim-legacy             Use the old aiming (nearest target, newest position, no reaction/error model)\n"
        "  --team <1|2>             Every bot requests this team (Karkand: 1 = MEC, 2 = USMC; default alternates)\n"
        "  --goal <x,y,z>           Every bot moves on this world position instead of choosing flags\n"
        "  --spawn-near <x,y,z>     Spawn at the friendly spawn group closest to this position\n"
        "  --keep-running           Never stand still; alternate sprint and slow walk every 2-4 s\n"
        "  --report-sec <int>      Periodic telemetry report interval in seconds (default: 10)\n"
        "  --verbose                Enable verbose per-bot handshake/disconnect logging\n"
        "  -h, --help               Show this help message\n",
        prog);
}

bool parseArgs(int argc, char** argv, RunnerOptions& opt, bool& showHelp) {
    showHelp = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            showHelp = true;
            return true;
        }
        if (arg == "--aim-legacy") {
            opt.aim.legacy = true;
            continue;
        }
        if (arg == "--team" && i + 1 < argc) {
            opt.forceTeam = static_cast<int32_t>(std::strtol(argv[++i], nullptr, 10));
            if (opt.forceTeam != 1 && opt.forceTeam != 2) {
                std::fprintf(stderr, "Invalid --team (1 = team 1/MEC on Karkand, 2 = team 2/USMC)\n");
                return false;
            }
            continue;
        }
        if (arg == "--goal" && i + 1 < argc) {
            // x,y,z world position every bot walks to instead of choosing flags.
            if (std::sscanf(argv[++i], "%f,%f,%f", &opt.aim.goalX, &opt.aim.goalY, &opt.aim.goalZ) != 3) {
                std::fprintf(stderr, "Invalid --goal (expected x,y,z)\n");
                return false;
            }
            opt.aim.hasGoal = true;
            continue;
        }
        if (arg == "--keep-running") {
            opt.aim.keepRunning = true;
            continue;
        }
        if (arg == "--spawn-near" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%f,%f,%f", &opt.aim.spawnX, &opt.aim.spawnY, &opt.aim.spawnZ) != 3) {
                std::fprintf(stderr, "Invalid --spawn-near (expected x,y,z)\n");
                return false;
            }
            opt.aim.hasSpawnNear = true;
            continue;
        }
        if (arg == "--verbose") {
            opt.verbose = true;
            continue;
        }

        if (i + 1 >= argc) {
            std::fprintf(stderr, "Missing value for option: %s\n", arg.c_str());
            return false;
        }
        const char* val = argv[++i];

        if (arg == "--host") {
            opt.host = val;
        } else if (arg == "--port") {
            const long p = std::strtol(val, nullptr, 10);
            if (p <= 0 || p > 65535) {
                std::fprintf(stderr, "Invalid --port: %s\n", val);
                return false;
            }
            opt.port = static_cast<uint16_t>(p);
        } else if (arg == "--bots") {
            const long b = std::strtol(val, nullptr, 10);
            if (b < 1 || b > 64) {
                std::fprintf(stderr, "Invalid --bots (must be 1..64): %s\n", val);
                return false;
            }
            opt.bots = static_cast<uint32_t>(b);
        } else if (arg == "--ping-ms") {
            opt.pingMs = std::strtod(val, nullptr);
            if (opt.pingMs < 0.0) opt.pingMs = 0.0;
        } else if (arg == "--ping-min-ms") {
            opt.pingMinMs = std::strtod(val, nullptr);
            if (opt.pingMinMs < 0.0) opt.pingMinMs = 0.0;
        } else if (arg == "--ping-max-ms") {
            opt.pingMaxMs = std::strtod(val, nullptr);
            if (opt.pingMaxMs < 0.0) opt.pingMaxMs = 0.0;
        } else if (arg == "--high-ping-pct") {
            opt.highPingPct = std::strtod(val, nullptr);
            if (opt.highPingPct < 0.0) opt.highPingPct = 0.0;
            if (opt.highPingPct > 100.0) opt.highPingPct = 100.0;
        } else if (arg == "--high-ping-min-ms") {
            opt.highPingMinMs = std::strtod(val, nullptr);
            if (opt.highPingMinMs < 0.0) opt.highPingMinMs = 0.0;
        } else if (arg == "--high-ping-max-ms") {
            opt.highPingMaxMs = std::strtod(val, nullptr);
            if (opt.highPingMaxMs < 0.0) opt.highPingMaxMs = 0.0;
        } else if (arg == "--jitter-ms") {
            opt.jitterMs = std::strtod(val, nullptr);
            if (opt.jitterMs < 0.0) opt.jitterMs = 0.0;
        } else if (arg == "--loss-pct") {
            opt.lossPct = std::strtod(val, nullptr);
            if (opt.lossPct < 0.0) opt.lossPct = 0.0;
            if (opt.lossPct > 100.0) opt.lossPct = 100.0;
        } else if (arg == "--duration-sec") {
            opt.durationSec = static_cast<int>(std::strtol(val, nullptr, 10));
            if (opt.durationSec < 0) opt.durationSec = 0;
        } else if (arg == "--navmesh") {
            opt.navmeshPath = val;
        } else if (arg == "--routes") {
            opt.routesPath = val;
        } else if (arg == "--mod") {
            opt.modName = val;
        } else if (arg == "--password") {
            opt.password = val;
        } else if (arg == "--stagger-ms") {
            opt.staggerMs = static_cast<int>(std::strtol(val, nullptr, 10));
            if (opt.staggerMs < 0) opt.staggerMs = 0;
        } else if (arg == "--world-size") {
            opt.worldSize = static_cast<float>(std::strtod(val, nullptr));
            if (opt.worldSize < 64.0f) opt.worldSize = 64.0f;
            opt.worldSizeExplicit = true;
        } else if (arg == "--aim-skill") {
            opt.aim.skill = static_cast<float>(std::strtod(val, nullptr));
            if (opt.aim.skill < 0.0f) opt.aim.skill = 0.0f;
            if (opt.aim.skill > 1.0f) opt.aim.skill = 1.0f;
        } else if (arg == "--interp-ms") {
            opt.aim.interpMs = static_cast<int32_t>(std::strtol(val, nullptr, 10));
            if (opt.aim.interpMs < 0) opt.aim.interpMs = 0;
            if (opt.aim.interpMs > 1000) opt.aim.interpMs = 1000;
        } else if (arg == "--seed") {
            opt.seed = static_cast<uint32_t>(std::strtoul(val, nullptr, 10));
        } else if (arg == "--report-sec") {
            opt.reportSec = static_cast<int>(std::strtol(val, nullptr, 10));
            if (opt.reportSec < 1) opt.reportSec = 1;
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

bool resolveServerAddress(const std::string& host, uint16_t port, sockaddr_in& outAddr) {
    std::memset(&outAddr, 0, sizeof(outAddr));
    outAddr.sin_family = AF_INET;
    outAddr.sin_port = htons(port);

    if (::inet_pton(AF_INET, host.c_str(), &outAddr.sin_addr) == 1) {
        return true;
    }

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    char portStr[16];
    std::snprintf(portStr, sizeof(portStr), "%u", static_cast<unsigned>(port));

    addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), portStr, &hints, &res);
    if (rc != 0 || !res) {
#ifdef _WIN32
        std::fprintf(stderr, "Failed to resolve host '%s': error %d\n", host.c_str(), rc);
#else
        std::fprintf(stderr, "Failed to resolve host '%s': %s\n", host.c_str(), ::gai_strerror(rc));
#endif
        return false;
    }

    const sockaddr_in* resolved = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
    outAddr = *resolved;
    ::freeaddrinfo(res);
    return true;
}

void updateBotCounts(const std::vector<std::unique_ptr<bf2bot::LoadBotClient> >& bots,
                     bf2bot::Telemetry& telemetry) {
    const uint32_t configured = static_cast<uint32_t>(bots.size());
    uint32_t connected = 0u;
    uint32_t spawned = 0u;
    uint32_t alive = 0u;
    for (size_t i = 0; i < bots.size(); ++i) {
        if (bots[i]->isConnected()) ++connected;
        if (bots[i]->isSpawned()) ++spawned;
        if (bots[i]->isAlive()) ++alive;
    }
    telemetry.setBotCounts(configured, connected, spawned, alive);
}

} // namespace

int main(int argc, char** argv) {
    RunnerOptions opt;
    bool showHelp = false;
    if (!parseArgs(argc, argv, opt, showHelp)) {
        printUsage(argv[0]);
        return 1;
    }
    if (showHelp) {
        printUsage(argv[0]);
        return 0;
    }

#ifdef _WIN32
    WinsockGuard winsock;
    if (!winsock.ok) {
        std::fprintf(stderr, "[LoadBot] WSAStartup failed\n");
        return 1;
    }
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
#else
    // Install POSIX signal handlers for graceful disconnect + final summary.
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handleSignal;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
#endif

    sockaddr_in serverAddr;
    if (!resolveServerAddress(opt.host, opt.port, serverAddr)) {
        return 1;
    }

    // Optional Tier 1 NavMesh
    bf2bot::NavMesh navMesh;
    if (!opt.navmeshPath.empty()) {
        if (navMesh.load(opt.navmeshPath)) {
            if (!opt.worldSizeExplicit && navMesh.worldSize() > 64.0f) {
                opt.worldSize = navMesh.worldSize();
            }
            std::printf("[LoadBot] Loaded NavMesh: %s (polys=%u, worldSize=%.1fm, AHM=%s)\n",
                        opt.navmeshPath.c_str(),
                        navMesh.polygonCount(),
                        navMesh.worldSize(),
                        navMesh.hasAerialHeightMap() ? "yes" : "no");
        } else {
            std::fprintf(stderr, "[LoadBot] Warning: failed to load NavMesh '%s'; falling back.\n",
                         opt.navmeshPath.c_str());
        }
    }

    // Optional Tier 2 RouteLoader
    bf2bot::RouteLoader routeLoader;
    if (!opt.routesPath.empty()) {
        if (routeLoader.load(opt.routesPath)) {
            std::printf("[LoadBot] Loaded Routes: %s (tracks=%zu, samples=%zu, map=%s)\n",
                        opt.routesPath.c_str(),
                        routeLoader.trackCount(),
                        routeLoader.sampleCount(),
                        routeLoader.mapName().c_str());
        } else {
            std::fprintf(stderr, "[LoadBot] Warning: failed to load Routes '%s'; falling back.\n",
                         opt.routesPath.c_str());
        }
    }

    if (opt.aim.legacy) {
        std::printf("[LoadBot] Aim: legacy (nearest target, newest position)\n");
    } else {
        std::printf("[LoadBot] Aim: human model skill=%.2f interp=%dms (LOS=%s)\n",
                    static_cast<double>(opt.aim.skill), static_cast<int>(opt.aim.interpMs),
                    navMesh.hasAerialHeightMap() ? "aerial-heightmap-bump" : "off(no AHM)");
    }

    bf2bot::Telemetry telemetry;
    const uint64_t startUs = steadyNowUs();

    std::vector<std::unique_ptr<bf2bot::LoadBotClient> > bots;
    bots.reserve(opt.bots);

    // Per-bot RTT is a seeded, deterministic draw so one process can model a mixed-ping lobby;
    // several processes would reuse the same bot names and profile ids and kick each other.
    std::vector<double> botPingMs(opt.bots, opt.pingMs);
    std::vector<bool> botHighPing(opt.bots, false);
    const bool perBotPing = opt.pingMinMs >= 0.0 && opt.pingMaxMs >= 0.0;
    if (perBotPing || opt.highPingPct > 0.0) {
        std::mt19937 pingRng(opt.seed ^ 0x5EEDF00Du);
        const double lo = perBotPing ? std::min(opt.pingMinMs, opt.pingMaxMs) : opt.pingMs;
        const double hi = perBotPing ? std::max(opt.pingMinMs, opt.pingMaxMs) : opt.pingMs;
        std::uniform_real_distribution<double> lowDist(lo, hi);
        std::uniform_real_distribution<double> highDist(std::min(opt.highPingMinMs, opt.highPingMaxMs),
                                                        std::max(opt.highPingMinMs, opt.highPingMaxMs));
        std::vector<uint32_t> order(opt.bots);
        for (uint32_t i = 0; i < opt.bots; ++i) order[i] = i;
        std::shuffle(order.begin(), order.end(), pingRng);
        const uint32_t highCount = static_cast<uint32_t>(opt.bots * opt.highPingPct / 100.0 + 0.5);
        for (uint32_t k = 0; k < opt.bots; ++k) {
            const uint32_t b = order[k];
            botHighPing[b] = k < highCount;
            botPingMs[b] = botHighPing[b] ? highDist(pingRng) : lowDist(pingRng);
        }
        std::printf("[LoadBot] Per-bot RTT ms (%u high-ping, marked *):", highCount);
        for (uint32_t b = 0; b < opt.bots; ++b)
            std::printf(" %02u=%.0f%s", b + 1u, botPingMs[b], botHighPing[b] ? "*" : "");
        std::printf("\n");
    }

    for (uint32_t i = 0; i < opt.bots; ++i) {
        bf2bot::LoadBotConfig cfg;
        cfg.botIndex = i;
        cfg.modName = opt.modName;
        cfg.password = opt.password;
        cfg.worldSize = opt.worldSize;
        cfg.seed = opt.seed;
        cfg.startAtUs = startUs + static_cast<uint64_t>(i) * static_cast<uint64_t>(opt.staggerMs) * 1000ULL;
        cfg.verbose = opt.verbose;
        cfg.aim = opt.aim;
        cfg.forceTeam = opt.forceTeam;

        // Split ping and jitter 50/50 across uplink and downlink so RTT matches --ping-ms / --jitter-ms.
        cfg.uplinkImpair.oneWayDelayMs = botPingMs[i] * 0.5;
        cfg.uplinkImpair.jitterMs = opt.jitterMs * 0.5;
        cfg.uplinkImpair.lossPct = opt.lossPct;
        cfg.uplinkImpair.seed = opt.seed ^ (i * 2u + 1u);

        cfg.downlinkImpair.oneWayDelayMs = botPingMs[i] * 0.5;
        cfg.downlinkImpair.jitterMs = opt.jitterMs * 0.5;
        cfg.downlinkImpair.lossPct = opt.lossPct;
        cfg.downlinkImpair.seed = opt.seed ^ (i * 2u + 2u);

        std::unique_ptr<bf2bot::LoadBotClient> bot(new bf2bot::LoadBotClient(
            cfg, serverAddr,
            navMesh.isLoaded() ? &navMesh : nullptr,
            routeLoader.isLoaded() ? &routeLoader : nullptr,
            &telemetry));

        if (!bot->initSocket()) {
#ifdef _WIN32
            std::fprintf(stderr, "[LoadBot] Failed to create UDP socket for bot %u (wsaErr=%d)\n",
                         i, ::WSAGetLastError());
#else
            std::fprintf(stderr, "[LoadBot] Failed to create UDP socket for bot %u (errno=%d)\n",
                         i, errno);
#endif
            return 1;
        }
        bots.push_back(std::move(bot));
    }

    char ipStr[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &serverAddr.sin_addr, ipStr, sizeof(ipStr));
    std::printf("[LoadBot] Starting %u bots -> %s:%u (ping=%.1fms jitter=%.1fms loss=%.2f%%/dir duration=%ds stagger=%dms world=%.0fm)\n",
                opt.bots, ipStr, static_cast<unsigned>(opt.port),
                opt.pingMs, opt.jitterMs, opt.lossPct,
                opt.durationSec, opt.staggerMs, opt.worldSize);
    std::fflush(stdout);

    static const uint64_t kTickPeriodUs = 33333ULL; // 30 Hz
    const uint64_t endUs = (opt.durationSec > 0)
        ? (startUs + static_cast<uint64_t>(opt.durationSec) * 1000000ULL)
        : std::numeric_limits<uint64_t>::max();
    const uint64_t reportPeriodUs = static_cast<uint64_t>(opt.reportSec) * 1000000ULL;

    uint64_t nextTickUs = startUs;
    uint64_t lastTickUs = startUs;
    uint64_t lastReportUs = startUs;
    uint64_t nextReportUs = startUs + reportPeriodUs;

#ifdef _WIN32
    std::vector<WSAPOLLFD> pollFds(bots.size());
#else
    std::vector<pollfd> pollFds(bots.size());
#endif

    while (!g_stopRequested) {
        const uint64_t nowUs = steadyNowUs();
        if (nowUs >= endUs) {
            break;
        }

        // Compute next wake deadline across 30 Hz tick, impairment queues, report, and end time.
        uint64_t nextWakeUs = nextTickUs;
        if (nextReportUs < nextWakeUs) nextWakeUs = nextReportUs;
        if (endUs < nextWakeUs) nextWakeUs = endUs;
        for (size_t i = 0; i < bots.size(); ++i) {
            const uint64_t bw = bots[i]->nextWakeUs();
            if (bw < nextWakeUs) {
                nextWakeUs = bw;
            }
        }

        int timeoutMs = 0;
        if (nextWakeUs > nowUs) {
            const uint64_t diffUs = nextWakeUs - nowUs;
            timeoutMs = static_cast<int>((diffUs + 999ULL) / 1000ULL);
            if (timeoutMs > 50) {
                timeoutMs = 50;
            }
        }

        for (size_t i = 0; i < bots.size(); ++i) {
            pollFds[i].fd = bots[i]->socketFd();
            pollFds[i].events = POLLIN;
            pollFds[i].revents = 0;
        }

#ifdef _WIN32
        const int ready = ::WSAPoll(pollFds.data(), static_cast<ULONG>(pollFds.size()), timeoutMs);
#else
        const int ready = ::poll(pollFds.data(), static_cast<nfds_t>(pollFds.size()), timeoutMs);
#endif
        const uint64_t afterPollUs = steadyNowUs();

        if (ready > 0) {
            for (size_t i = 0; i < bots.size(); ++i) {
                if ((pollFds[i].revents & POLLIN) != 0) {
                    bots[i]->onSocketReadable(afterPollUs);
                }
            }
        }

        // Flush any impairment-delayed uplink/downlink packets whose delivery time has arrived.
        for (size_t i = 0; i < bots.size(); ++i) {
            bots[i]->flushImpairmentQueues(afterPollUs);
        }

        // Fixed 30 Hz bot simulation & packet transmit step.
        if (afterPollUs >= nextTickUs) {
            const float dtSec = static_cast<float>(afterPollUs - lastTickUs) / 1000000.0f;
            lastTickUs = afterPollUs;
            for (size_t i = 0; i < bots.size(); ++i) {
                bots[i]->tick30Hz(afterPollUs, dtSec);
            }
            nextTickUs += kTickPeriodUs;
            if (nextTickUs + kTickPeriodUs * 4ULL < afterPollUs) {
                nextTickUs = afterPollUs + kTickPeriodUs; // prevent spiral after pause
            }
        }

        // Periodic telemetry report.
        if (afterPollUs >= nextReportUs) {
            updateBotCounts(bots, telemetry);
            const double elapsedSec = static_cast<double>(afterPollUs - startUs) / 1000000.0;
            const double windowSec = static_cast<double>(afterPollUs - lastReportUs) / 1000000.0;
            std::printf("%s\n", telemetry.formatReport("INTERVAL", elapsedSec, windowSec).c_str());
            std::fflush(stdout);
            telemetry.resetWindow();
            lastReportUs = afterPollUs;
            nextReportUs = afterPollUs + reportPeriodUs;
        }
    }

    // Graceful shutdown: send 3x Disconnect for every bot and print final A/B summary.
    const uint64_t finishUs = steadyNowUs();
    updateBotCounts(bots, telemetry);
    for (size_t i = 0; i < bots.size(); ++i) {
        bots[i]->disconnectGracefully();
    }

    const double totalElapsedSec = static_cast<double>(finishUs - startUs) / 1000000.0;
    const double lastWindowSec = static_cast<double>(finishUs - lastReportUs) / 1000000.0;
    std::printf("\n%s\n", telemetry.formatReport("FINAL A/B SUMMARY", totalElapsedSec, lastWindowSec).c_str());
    std::fflush(stdout);
    return 0;
}
