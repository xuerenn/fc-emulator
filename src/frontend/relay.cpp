// relay.cpp — FC 模拟器联机中继服务器
//
// 为什么需要它：
//   两台都在家庭/公司 NAT 后面的机器无法互相直连。让双方都主动连到一台
//   公网服务器，由服务器把两边配对并互相转发，就能做到「零配置联机」。
//
// 它是**透明**的：配对之后只做「收到 A 的包原样发给 B」这一件事，
// 完全不需要理解模拟器的同步协议。所以模拟器侧的握手与锁步逻辑一行没改。
//
// 编译（Linux / macOS / MinGW 均可，无第三方依赖）：
//     g++ -O2 -std=c++17 -o fc-relay relay.cpp
//   本目录的中继源码是单文件自包含的，只需连同 net/relay_proto.h 一起拷到服务器；
//   也可以直接在本工程用 CMake 构建出 fc-relay(.exe)。
//
// 运行：
//     ./fc-relay 7777                 # 监听 UDP 7777（默认）
//     ./fc-relay --bind 0.0.0.0 -p 7777
//
// 服务器只需放行这一个 UDP 端口。
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "net/relay_proto.h"

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  using raw_socket = SOCKET;
  static const raw_socket kInvalidSock = INVALID_SOCKET;
  #define FC_CLOSE_SOCK(s)   ::closesocket(s)
  #define FC_WOULD_BLOCK(e)  ((e) == WSAEWOULDBLOCK || (e) == WSAETIMEDOUT || (e) == WSAEINTR)
  static int lastSockErr() { return ::WSAGetLastError(); }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <errno.h>
  #include <time.h>
  using raw_socket = int;
  static const raw_socket kInvalidSock = -1;
  #define FC_CLOSE_SOCK(s)   ::close(s)
  #define FC_WOULD_BLOCK(e)  ((e) == EAGAIN || (e) == EWOULDBLOCK || (e) == EINTR)
  static int lastSockErr() { return errno; }
#endif

namespace {

using fc::relay::Header;

constexpr int kMaxRooms = 256;
constexpr std::uint32_t kRoomIdleMs  = 60000;   // 房间整体 60s 无活动 -> 回收
constexpr std::uint32_t kSlotStaleMs = 5000;    // 单端 5s 无活动 -> 允许新地址顶替
constexpr std::uint32_t kStatsMs     = 30000;   // 每 30s 打一行统计
constexpr int kRecvTimeoutMs = 500;             // 收包超时，让主循环能跑回收与统计

struct Endpoint {
    sockaddr_in   addr{};
    bool          used = false;
    std::uint32_t lastSeen = 0;
    std::uint64_t packets = 0;
};

struct Room {
    bool          used = false;
    char          id[fc::relay::kRoomLen] = {0};
    Endpoint      ep[2];
    std::uint32_t lastSeen = 0;
    std::uint64_t forwarded = 0;
    bool          paired = false;   // 仅用于日志去重
};

std::uint32_t nowMs() {
#ifdef _WIN32
    return std::uint32_t(::GetTickCount());
#else
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return std::uint32_t(std::uint64_t(ts.tv_sec) * 1000ull
                       + std::uint64_t(ts.tv_nsec) / 1000000ull);
#endif
}

bool sameAddr(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

void formatAddr(const sockaddr_in& a, char* out, int n) {
    char ip[32] = {0};
    std::snprintf(ip, sizeof(ip), "%s", ::inet_ntoa(a.sin_addr));
    std::snprintf(out, size_t(n), "%s:%u", ip, unsigned(::ntohs(a.sin_port)));
}

const char* roleName(std::uint8_t r) {
    switch (r) {
        case fc::relay::RL_ROLE_HOST:   return "主机";
        case fc::relay::RL_ROLE_CLIENT: return "客机";
        default:                        return "未知";
    }
}

// ---------------------------------------------------------------- 中继主体
class Relay {
public:
    bool open(const std::string& bindIp, std::uint16_t port) {
#ifdef _WIN32
        WSADATA w;
        if (::WSAStartup(MAKEWORD(2, 2), &w) != 0) {
            std::fprintf(stderr, "[relay] WSAStartup 失败\n");
            return false;
        }
        int timeout = kRecvTimeoutMs;
        const char* toPtr = reinterpret_cast<const char*>(&timeout);
        int toLen = sizeof(timeout);
#else
        struct timeval tv;
        tv.tv_sec  = kRecvTimeoutMs / 1000;
        tv.tv_usec = (kRecvTimeoutMs % 1000) * 1000;
        const char* toPtr = reinterpret_cast<const char*>(&tv);
        int toLen = sizeof(tv);
#endif

        raw_socket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == kInvalidSock) { std::fprintf(stderr, "[relay] socket() 失败\n"); return false; }

        int one = 1;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, toPtr, toLen);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        if (bindIp.empty() || bindIp == "0.0.0.0") {
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        } else if (::inet_addr(bindIp.c_str()) == INADDR_NONE) {
            std::fprintf(stderr, "[relay] 无效的绑定地址: %s（只支持点分十进制 IPv4）\n", bindIp.c_str());
            FC_CLOSE_SOCK(s);
            return false;
        } else {
            addr.sin_addr.s_addr = ::inet_addr(bindIp.c_str());
        }

        if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            std::fprintf(stderr, "[relay] 绑定 %s:%u 失败（端口被占用或权限不足）\n",
                         bindIp.empty() ? "0.0.0.0" : bindIp.c_str(), unsigned(port));
            FC_CLOSE_SOCK(s);
            return false;
        }

        sock_ = s;
        startedMs_ = nowMs();
        std::printf("[relay] fc-relay 已启动，监听 UDP %s:%u\n",
                    bindIp.empty() ? "0.0.0.0" : bindIp.c_str(), unsigned(port));
        std::printf("[relay] 玩家用法: fc-emulator <rom> --relay <本机公网IP>:%u --room <房间号> --host|--connect\n",
                    unsigned(port));
        std::printf("[relay] 注意: 防火墙需放行 UDP %u（不是 TCP）\n\n", unsigned(port));
        std::fflush(stdout);
        return true;
    }

    // 一直跑到进程被结束
    void run() {
        std::uint8_t buf[2048];
        std::uint32_t lastStats = nowMs();

        for (;;) {
            sockaddr_in from{};
#ifdef _WIN32
            int alen = sizeof(from);
#else
            socklen_t alen = sizeof(from);
#endif
            const int n = int(::recvfrom(sock_, reinterpret_cast<char*>(buf), int(sizeof(buf)), 0,
                                         reinterpret_cast<sockaddr*>(&from), &alen));
            const std::uint32_t t = nowMs();

            if (n < 0) {
                const int e = lastSockErr();
                if (!FC_WOULD_BLOCK(e)) {
                    std::fprintf(stderr, "[relay] recvfrom 出错 (%d)，继续运行\n", e);
                }
            } else if (n >= int(sizeof(Header))
                       && std::memcmp(buf, fc::relay::kMagic, 4) == 0) {
                Header h;
                std::memcpy(&h, buf, sizeof(h));
                if (h.version == fc::relay::kVersion) handleControl(h, from, t);
            } else if (n > 0) {
                forward(buf, n, from, t);
            }

            if (t - lastStats >= kStatsMs) { lastStats = t; printStats(t); }
            gc(t);
        }
    }

private:
    // ------------------------------------------------------------ 房间查找
    Room* findRoomById(const char* id) {
        for (int i = 0; i < kMaxRooms; ++i) {
            if (rooms_[i].used && std::strncmp(rooms_[i].id, id, fc::relay::kRoomLen - 1) == 0) {
                return &rooms_[i];
            }
        }
        return nullptr;
    }

    Room* findRoomByAddr(const sockaddr_in& a, int* outSlot) {
        for (int i = 0; i < kMaxRooms; ++i) {
            if (!rooms_[i].used) continue;
            for (int k = 0; k < 2; ++k) {
                if (rooms_[i].ep[k].used && sameAddr(rooms_[i].ep[k].addr, a)) {
                    if (outSlot) *outSlot = k;
                    return &rooms_[i];
                }
            }
        }
        return nullptr;
    }

    Room* allocRoom(const char* id) {
        for (int i = 0; i < kMaxRooms; ++i) {
            if (!rooms_[i].used) {
                rooms_[i] = Room{};
                rooms_[i].used = true;
                std::snprintf(rooms_[i].id, fc::relay::kRoomLen, "%s", id);
                return &rooms_[i];
            }
        }
        return nullptr;
    }

    int slotOf(Room* r, const sockaddr_in& a) {
        for (int k = 0; k < 2; ++k) {
            if (r->ep[k].used && sameAddr(r->ep[k].addr, a)) return k;
        }
        return -1;
    }

    // ------------------------------------------------------------ 控制报文
    void sendControl(const sockaddr_in& to, std::uint8_t type) {
        Header h{};
        std::memcpy(h.magic, fc::relay::kMagic, 4);
        h.version = fc::relay::kVersion;
        h.type    = type;
        ::sendto(sock_, reinterpret_cast<const char*>(&h), int(sizeof(h)), 0,
                 reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }

    void handleControl(Header h, const sockaddr_in& from, std::uint32_t t) {
        if (h.type == fc::relay::RL_UNREGISTER) {
            int slot = -1;
            Room* r = findRoomByAddr(from, &slot);
            if (!r) return;
            char who[32];
            formatAddr(from, who, sizeof(who));
            std::printf("[relay] 房间 %s: %s 离开\n", r->id, who);
            std::fflush(stdout);
            r->ep[slot] = Endpoint{};
            r->paired = false;
            if (!r->ep[0].used && !r->ep[1].used) {
                r->used = false;
                std::printf("[relay] 房间 %s: 已回收\n", r->id);
                std::fflush(stdout);
            }
            return;
        }

        if (h.type != fc::relay::RL_REGISTER) return;

        h.room[fc::relay::kRoomLen - 1] = '\0';
        const char* id = (h.room[0] != '\0') ? h.room : "default";

        // 同一个地址不要被登记进两个房间
        int existing = -1;
        Room* r = findRoomByAddr(from, &existing);
        if (!r) r = findRoomById(id);
        if (!r) r = allocRoom(id);
        if (!r) {
            char who[32];
            formatAddr(from, who, sizeof(who));
            std::fprintf(stderr, "[relay] 房间数已达上限 %d，拒绝 %s 的注册\n", kMaxRooms, who);
            sendControl(from, fc::relay::RL_BUSY);
            return;
        }

        int slot = slotOf(r, from);
        bool fresh = false;
        if (slot < 0) {
            for (int k = 0; k < 2; ++k) {
                if (!r->ep[k].used) { slot = k; break; }
            }
        }
        if (slot < 0) {
            // 两个槽都在用：允许顶替「静默」的那一个（对端换端口重连的常见情形）
            for (int k = 0; k < 2; ++k) {
                if (t - r->ep[k].lastSeen > kSlotStaleMs) { slot = k; break; }
            }
            if (slot < 0) {
                char who[32];
                formatAddr(from, who, sizeof(who));
                std::fprintf(stderr, "[relay] 房间 %s 已被占用，拒绝 %s（%s）\n",
                             r->id, who, roleName(h.role));
                std::fflush(stderr);
                sendControl(from, fc::relay::RL_BUSY);
                return;
            }
            char old[32], now2[32];
            formatAddr(r->ep[slot].addr, old, sizeof(old));
            formatAddr(from, now2, sizeof(now2));
            std::printf("[relay] 房间 %s: %s 静默超时，由 %s 顶替（%s）\n",
                        r->id, old, now2, roleName(h.role));
            r->paired = false;
        }

        if (!r->ep[slot].used) fresh = true;
        r->ep[slot].used     = true;
        r->ep[slot].addr     = from;
        r->ep[slot].lastSeen = t;
        if (fresh) r->ep[slot].packets = 0;
        r->lastSeen = t;

        if (r->ep[0].used && r->ep[1].used) {
            if (!r->paired) {
                char a[32], b[32];
                formatAddr(r->ep[0].addr, a, sizeof(a));
                formatAddr(r->ep[1].addr, b, sizeof(b));
                std::printf("[relay] 房间 %s: 已配对  %s <-> %s\n", r->id, a, b);
                std::printf("[relay] 房间 %s: 开始中转（双方后续可见「已连接」）\n", r->id);
                std::fflush(stdout);
                r->paired = true;
            }
            // 每次都重发 PAIR：任一端的注册重试都会触发，天然抗丢包
            sendControl(r->ep[0].addr, fc::relay::RL_PAIR);
            sendControl(r->ep[1].addr, fc::relay::RL_PAIR);
        } else {
            sendControl(from, fc::relay::RL_WAIT);
        }
    }

    // ------------------------------------------------------------ 数据转发
    void forward(const std::uint8_t* buf, int n, const sockaddr_in& from, std::uint32_t t) {
        int slot = -1;
        Room* r = findRoomByAddr(from, &slot);
        if (!r) { dropped_++; return; }

        r->ep[slot].lastSeen = t;
        r->ep[slot].packets++;
        r->lastSeen = t;

        const int other = 1 - slot;
        if (!r->ep[other].used) { dropped_++; return; }

        ::sendto(sock_, reinterpret_cast<const char*>(buf), n, 0,
                 reinterpret_cast<const sockaddr*>(&r->ep[other].addr),
                 sizeof(r->ep[other].addr));
        r->forwarded++;
        totalForwarded_++;
    }

    // ------------------------------------------------------------ 回收与统计
    void gc(std::uint32_t t) {
        for (int i = 0; i < kMaxRooms; ++i) {
            Room& r = rooms_[i];
            if (!r.used) continue;
            for (int k = 0; k < 2; ++k) {
                if (!r.ep[k].used) continue;
                if (t - r.ep[k].lastSeen > kRoomIdleMs) {
                    char who[32];
                    formatAddr(r.ep[k].addr, who, sizeof(who));
                    std::printf("[relay] 房间 %s: %s 超时（%llu 包），释放槽位\n",
                                r.id, who, static_cast<unsigned long long>(r.ep[k].packets));
                    std::fflush(stdout);
                    r.ep[k] = Endpoint{};
                    r.paired = false;
                }
            }
            if (!r.ep[0].used && !r.ep[1].used) {
                std::printf("[relay] 房间 %s: 已回收\n", r.id);
                std::fflush(stdout);
                r.used = false;
            }
        }
    }

    void printStats(std::uint32_t t) {
        int active = 0;
        for (int i = 0; i < kMaxRooms; ++i) if (rooms_[i].used) active++;
        if (active == 0 && totalForwarded_ == 0) return;

        int paired = 0;
        for (int i = 0; i < kMaxRooms; ++i) {
            if (rooms_[i].used && rooms_[i].ep[0].used && rooms_[i].ep[1].used) paired++;
        }
        const unsigned long mins = static_cast<unsigned long>((t - startedMs_) / 60000);
        std::printf("[relay] 运行 %lu 分 | 房间 %d（其中对战中 %d）| 累计转发 %llu 包 | 丢弃 %llu\n",
                    mins, active, paired,
                    static_cast<unsigned long long>(totalForwarded_),
                    static_cast<unsigned long long>(dropped_));
        std::fflush(stdout);
    }

    raw_socket sock_ = kInvalidSock;
    Room rooms_[kMaxRooms]{};
    std::uint32_t startedMs_ = 0;
    std::uint64_t totalForwarded_ = 0;
    std::uint64_t dropped_ = 0;
};

void usage() {
    std::printf(
        "fc-relay — FC 模拟器联机中继服务器\n"
        "\n"
        "用法:\n"
        "  fc-relay [端口] [选项]\n"
        "  fc-relay 7777\n"
        "  fc-relay --bind 0.0.0.0 --port 7777\n"
        "\n"
        "选项:\n"
        "  -p, --port <端口>    监听端口，默认 7777\n"
        "  -b, --bind <IP>      绑定地址，默认 0.0.0.0\n"
        "  -h, --help           显示本帮助\n"
        "\n"
        "说明:\n"
        "  服务器只需放行一个 UDP 端口。玩家两端都主动连过来，因此\n"
        "  双方都不需要公网 IP、不需要端口映射。\n");
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    ::SetConsoleOutputCP(CP_UTF8);   // 让中文日志在 Windows 控制台正常显示
#endif

    std::string bindIp = "0.0.0.0";
    std::uint16_t port = 7777;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto takeNext = [&](const char* def) -> std::string {
            if (i + 1 < argc && argv[i + 1][0] != '-') return argv[++i];
            return def;
        };
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "-p" || a == "--port") port = std::uint16_t(std::atoi(takeNext("7777").c_str()));
        else if (a == "-b" || a == "--bind") bindIp = takeNext("0.0.0.0");
        else if (!a.empty() && std::isdigit(static_cast<unsigned char>(a[0]))) {
            port = std::uint16_t(std::atoi(a.c_str()));
        } else {
            std::fprintf(stderr, "[relay] 无法识别的参数: %s\n", a.c_str());
            usage();
            return 1;
        }
    }

    if (port == 0) { std::fprintf(stderr, "[relay] 端口不能为 0\n"); return 1; }

    Relay relay;
    if (!relay.open(bindIp, port)) return 1;
    relay.run();
    return 0;
}
