// net.cpp — UDP 与锁步帧同步实现
#include "net.h"
#include "relay_proto.h"

#include <cstdio>
#include <cstring>

// Windows 下启用较新的网络/计时 API
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  using raw_socket = SOCKET;
  static constexpr raw_socket kInvalidSock = INVALID_SOCKET;
  static constexpr int kWouldBlock = WSAEWOULDBLOCK;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <time.h>
  using raw_socket = int;
  static constexpr raw_socket kInvalidSock = -1;
  static constexpr int kWouldBlock = EWOULDBLOCK;
#endif

namespace fc {

namespace {

void netInit() {
#ifdef _WIN32
    static bool done = false;
    if (!done) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        done = true;
    }
#endif
}

u32 nowMs() {
#ifdef _WIN32
    return u32(::GetTickCount());
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return u32(u64(ts.tv_sec) * 1000ull + u64(ts.tv_nsec) / 1000000ull);
#endif
}

void sleepMs(int ms) {
#ifdef _WIN32
    ::Sleep(DWORD(ms));
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = long(ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
#endif
}

// 只用点分十进制；额外接受 localhost 别名（不含 DNS 解析，避免阻塞）
bool parseIPv4(const std::string& host, in_addr& out) {
    std::string h = host;
    if (h.empty() || h == "localhost") h = "127.0.0.1";
    const unsigned long a = ::inet_addr(h.c_str());
    if (a == INADDR_NONE) return false;
    out.s_addr = a;
    return true;
}

// ---------------------------------------------------------------- 报文
#pragma pack(push, 1)
struct NetHeader {
    u8  magic[4];    // 'F','C','N','P'
    u8  version;
    u8  type;
    u8  count;
    u8  reserved;
    u32 slot;        // 发送方当前采样序号
};
struct NetEntry {
    u32 frame;
    u8  input;
    u8  pad[3];
};
struct NetHashPacket {
    NetHeader h;
    u32 frame;
    u32 hashHi;
    u32 hashLo;
};
#pragma pack(pop)

enum PacketType : u8 {
    PT_HELLO     = 1,
    PT_HELLO_ACK = 2,
    PT_CONFIRM   = 3,
    PT_INPUT     = 4,
    PT_HASH      = 5,
    PT_BYE       = 6,
};

} // namespace

// ================================================================ UdpSocket
UdpSocket::UdpSocket() { netInit(); }

UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::valid() const {
    return sock_ != -1 && sock_ != std::intptr_t(kInvalidSock);
}

bool UdpSocket::open(u16 bindPort) {
    close();
    raw_socket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalidSock) return false;

    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(bindPort);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
#ifdef _WIN32
        ::closesocket(s);
#else
        ::close(s);
#endif
        return false;
    }

#ifdef _WIN32
    u_long nb = 1;
    ::ioctlsocket(s, FIONBIO, &nb);
#else
    int fl = ::fcntl(s, F_GETFL, 0);
    ::fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif

    sock_ = std::intptr_t(s);
    return true;
}

void UdpSocket::close() {
    if (!valid()) { sock_ = -1; return; }
    raw_socket s = raw_socket(sock_);
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
    sock_ = -1;
}

bool UdpSocket::sendTo(const std::string& host, u16 port, const u8* data, int n) {
    if (!valid()) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (!parseIPv4(host, addr.sin_addr)) return false;
    const int r = int(::sendto(raw_socket(sock_), reinterpret_cast<const char*>(data), n, 0,
                               reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
    return r == n;
}

int UdpSocket::recvFrom(std::string& outHost, u16& outPort, u8* data, int maxN) {
    if (!valid()) return -1;
    sockaddr_in addr{};
#ifdef _WIN32
    int alen = sizeof(addr);
#else
    socklen_t alen = sizeof(addr);
#endif
    const int r = int(::recvfrom(raw_socket(sock_), reinterpret_cast<char*>(data), maxN, 0,
                                 reinterpret_cast<sockaddr*>(&addr), &alen));
    if (r < 0) {
#ifdef _WIN32
        if (WSAGetLastError() == kWouldBlock) return 0;
#else
        if (errno == kWouldBlock || errno == EAGAIN) return 0;
#endif
        return -1;
    }
    outHost = ::inet_ntoa(addr.sin_addr);
    outPort = ntohs(addr.sin_port);
    return r;
}

// ================================================================ NetSession
NetSession::~NetSession() { stop(); }

const char* netRoleName(NetSession::Role r) {
    switch (r) {
        case NetSession::Role::Host:   return "主机";
        case NetSession::Role::Client: return "客机";
        default:                       return "单机";
    }
}

// 握手成功后的公共收尾：清空输入缓冲与统计
void NetSession::resetStreamState() {
    slot_ = 0;
    remoteConsumed_ = 0;
    disconnected_ = false;
    desynced_ = false;
    lag_ = 0;
    local_.fill(Slot{});
    remote_.fill(Slot{});
    hashLocal_.fill(HashSlot{});
    hashRemote_.fill(HashSlot{});
    desyncInfo_ = DesyncInfo{};
    desyncPending_ = false;
    desyncCount_ = 0;
}

// 记录一次不同步。desynced_ 是「曾经不同步」的锁存位（标题栏用），
// desyncInfo_ 只保留第一次的详情供前端播报一次，避免每帧刷屏。
void NetSession::noteDesync(u32 frame, u64 mine, u64 theirs) {
    desynced_ = true;
    ++desyncCount_;
    if (!desyncPending_) {
        desyncInfo_ = DesyncInfo{ frame, mine, theirs };
        desyncPending_ = true;
    }
}

bool NetSession::takeDesyncEvent(DesyncInfo& out) {
    if (!desyncPending_) return false;
    out = desyncInfo_;
    desyncPending_ = false;
    return true;
}

bool NetSession::startHost(u16 port, int inputDelay, std::string* err, const WaitHook& onWait) {
    stop();
    strategy_.reset(new LockstepStrategy());
    if (!sock_.open(port)) {
        if (err) *err = "端口 " + std::to_string(port) + " 绑定失败";
        return false;
    }
    role_ = Role::Host;
    delay_ = inputDelay > 0 ? inputDelay : strategy_->defaultDelay();
    if (!handshakeHost(err, onWait)) { stop(); return false; }
    resetStreamState();
    return true;
}

bool NetSession::startClient(const std::string& host, u16 port, int inputDelay, std::string* err,
                            const WaitHook& onWait) {
    stop();
    strategy_.reset(new LockstepStrategy());
    if (!sock_.open(0)) {
        if (err) *err = "本地端口分配失败";
        return false;
    }
    role_ = Role::Client;
    delay_ = inputDelay > 0 ? inputDelay : strategy_->defaultDelay();
    if (!handshakeClient(host, port, err, onWait)) { stop(); return false; }
    resetStreamState();
    return true;
}

// ---------------------------------------------------------------- 中继模式
// 与直连的唯一区别：双方都不开固定端口，先向中继注册房间、等它把两端配对，
// 之后把「对端地址」当成中继地址即可——握手、锁步、哈希比对全部复用原逻辑。
bool NetSession::startHostViaRelay(const RelayConfig& relay, int inputDelay, std::string* err,
                                   const WaitHook& onWait) {
    stop();
    strategy_.reset(new LockstepStrategy());
    relayCfg_ = relay;
    relayed_ = true;
    relayReachable_ = false;
    if (!sock_.open(0)) {
        if (err) *err = "本地端口分配失败";
        relayed_ = false;
        relayCfg_ = RelayConfig{};
        return false;
    }
    role_ = Role::Host;
    delay_ = inputDelay > 0 ? inputDelay : kRelayDefaultDelay;
    if (!registerWithRelay(err, onWait)) { stop(); return false; }
    peerHost_ = relay.host;
    peerPort_ = relay.port;
    if (!handshakeHost(err, onWait)) { stop(); return false; }
    resetStreamState();
    return true;
}

bool NetSession::startClientViaRelay(const RelayConfig& relay, int inputDelay, std::string* err,
                                     const WaitHook& onWait) {
    stop();
    strategy_.reset(new LockstepStrategy());
    relayCfg_ = relay;
    relayed_ = true;
    relayReachable_ = false;
    if (!sock_.open(0)) {
        if (err) *err = "本地端口分配失败";
        relayed_ = false;
        relayCfg_ = RelayConfig{};
        return false;
    }
    role_ = Role::Client;
    delay_ = inputDelay > 0 ? inputDelay : kRelayDefaultDelay;
    if (!registerWithRelay(err, onWait)) { stop(); return false; }
    if (!handshakeClient(relay.host, relay.port, err, onWait)) { stop(); return false; }
    resetStreamState();
    return true;
}

void NetSession::sendRelayControl(u8 type) {
    if (!sock_.valid() || relayCfg_.host.empty()) return;
    relay::Header h{};
    std::memcpy(h.magic, relay::kMagic, 4);
    h.version = relay::kVersion;
    h.type    = type;
    h.role    = (role_ == Role::Host) ? relay::RL_ROLE_HOST : relay::RL_ROLE_CLIENT;
    std::snprintf(h.room, sizeof(h.room), "%s", relayCfg_.room.c_str());
    sock_.sendTo(relayCfg_.host, relayCfg_.port,
                 reinterpret_cast<const u8*>(&h), int(sizeof(h)));
}

bool NetSession::registerWithRelay(std::string* err, const WaitHook& onWait) {
    u8 buf[512];
    std::string rh;
    u16 rp = 0;
    u32 lastSend = 0;
    const u32 deadline = nowMs() + 60000;      // 最多等 1 分钟

    while (nowMs() < deadline) {
        if (onWait && !onWait()) {
            if (err) *err = "已取消等待";
            return false;
        }
        if (nowMs() - lastSend > 200) {        // 重发注册：兼作 NAT 保活与抗丢包
            sendRelayControl(relay::RL_REGISTER);
            lastSend = nowMs();
        }

        const int n = sock_.recvFrom(rh, rp, buf, sizeof(buf));
        if (n >= int(sizeof(relay::Header))) {
            relay::Header h{};
            std::memcpy(&h, buf, sizeof(h));
            if (std::memcmp(h.magic, relay::kMagic, 4) == 0 && h.version == relay::kVersion) {
                if (h.type == relay::RL_PAIR) { relayReachable_ = true; return true; }
                if (h.type == relay::RL_WAIT) { relayReachable_ = true; }
                if (h.type == relay::RL_BUSY) {
                    if (err) {
                        *err = "房间 " + relayCfg_.room +
                               " 已被占用（可能上一局的连接还没超时，换一个房间号即可）";
                    }
                    return false;
                }
            }
        }
        sleepMs(2);
    }

    if (err) {
        // 区分「服务器不通」和「对端没来」——这两种情况的排查方向完全不同
        *err = relayReachable_
             ? ("中继已收到注册，但房间 " + relayCfg_.room + " 里一直没人加入（对端要用同一个房间号）")
             : ("中继服务器无响应（检查 --relay 地址/端口，以及服务器是否放行 UDP）");
    }
    return false;
}

void NetSession::stop() {
    if (role_ != Role::None && !peerHost_.empty()) sendPacket(PT_BYE);
    // 先发 BYE 再注销：中继是按收到的先后顺序处理的，
    // 所以 BYE 一定已经被转发出去，不会因为槽位释放而丢失。
    if (relayed_) sendRelayControl(relay::RL_UNREGISTER);
    sock_.close();
    role_ = Role::None;
    peerHost_.clear();
    peerPort_ = 0;
    relayed_ = false;
    relayReachable_ = false;
    relayCfg_ = RelayConfig{};
}

bool NetSession::handshakeHost(std::string* err, const WaitHook& onWait) {
    u8 buf[512];
    std::string host;
    u16 port = 0;
    bool gotHello = false;
    u32 lastAck = 0;
    const u32 deadline = nowMs() + 120000;      // 最多等 2 分钟

    while (nowMs() < deadline) {
        if (onWait && !onWait()) {               // 前端泵事件 / 重绘，返回 false 即取消
            if (err) *err = "已取消等待";
            return false;
        }
        const int n = sock_.recvFrom(host, port, buf, sizeof(buf));
        if (n >= int(sizeof(NetHeader))) {
            NetHeader h;
            std::memcpy(&h, buf, sizeof(h));
            if (std::memcmp(h.magic, "FCNP", 4) == 0 && h.version == 1) {
                if (h.type == PT_HELLO) {
                    peerHost_ = host;
                    peerPort_ = port;
                    gotHello = true;
                    sendPacket(PT_HELLO_ACK);
                    lastAck = nowMs();
                } else if (h.type == PT_CONFIRM && gotHello &&
                           host == peerHost_ && port == peerPort_) {
                    return true;
                }
            }
        }
        if (gotHello && nowMs() - lastAck > 250) {
            sendPacket(PT_HELLO_ACK);
            lastAck = nowMs();
        }
        sleepMs(2);
    }
    if (err) *err = "等待客机连接超时";
    return false;
}

bool NetSession::handshakeClient(const std::string& host, u16 port, std::string* err,
                                 const WaitHook& onWait) {
    peerHost_ = host;
    peerPort_ = port;

    u8 buf[512];
    std::string rh;
    u16 rp = 0;
    u32 lastSend = 0;
    const u32 deadline = nowMs() + 15000;

    while (nowMs() < deadline) {
        if (onWait && !onWait()) {
            if (err) *err = "已取消连接";
            return false;
        }
        if (nowMs() - lastSend > 200) {
            sendPacket(PT_HELLO);
            lastSend = nowMs();
        }
        const int n = sock_.recvFrom(rh, rp, buf, sizeof(buf));
        if (n >= int(sizeof(NetHeader))) {
            NetHeader h;
            std::memcpy(&h, buf, sizeof(h));
            if (std::memcmp(h.magic, "FCNP", 4) == 0 && h.version == 1 && h.type == PT_HELLO_ACK) {
                peerHost_ = rh;
                peerPort_ = rp;
                for (int i = 0; i < 3; ++i) { sendPacket(PT_CONFIRM); sleepMs(4); }
                return true;
            }
        }
        sleepMs(2);
    }
    if (err) *err = "连接主机失败（请检查 IP/端口与防火墙）";
    return false;
}

bool NetSession::sendPacket(u8 type) {
    if (!sock_.valid() || peerHost_.empty()) return false;

    u8 buf[sizeof(NetHeader) + kRedundancy * sizeof(NetEntry)];
    NetHeader h{};
    std::memcpy(h.magic, "FCNP", 4);
    h.version = 1;
    h.type = type;
    h.count = 0;
    h.reserved = 0;
    h.slot = slot_;

    int off = int(sizeof(NetHeader));
    int count = 0;
    for (int i = 0; i < kRedundancy; ++i) {
        if (slot_ < u32(i)) break;
        const u32 f = slot_ - u32(i);
        const Slot& s = local_[f & MASK];
        if (s.frame != f) continue;
        NetEntry e{};
        e.frame = f;
        e.input = s.input;
        std::memcpy(buf + off, &e, sizeof(e));
        off += int(sizeof(e));
        count++;
    }
    h.count = u8(count);
    std::memcpy(buf, &h, sizeof(h));
    return sock_.sendTo(peerHost_, peerPort_, buf, off);
}

void NetSession::drainIncoming() {
    u8 buf[512];
    std::string host;
    u16 port = 0;

    for (int guard = 0; guard < 64; ++guard) {
        const int n = sock_.recvFrom(host, port, buf, sizeof(buf));
        if (n <= 0) break;
        if (n < int(sizeof(NetHeader))) continue;

        NetHeader h;
        std::memcpy(&h, buf, sizeof(h));
        if (std::memcmp(h.magic, "FCNP", 4) != 0 || h.version != 1) continue;
        if (!peerHost_.empty() && (host != peerHost_ || port != peerPort_)) continue;

        if (h.type == PT_BYE) { disconnected_ = true; return; }

        if (h.type == PT_HASH && n >= int(sizeof(NetHashPacket))) {
            NetHashPacket p;
            std::memcpy(&p, buf, sizeof(p));
            const u64 rh = (u64(p.hashHi) << 32) | u64(p.hashLo);
            // 只在「同帧」这一前提下比对才有意义。哈希条目自带帧号，
            // 所以环被复用后留下的旧条目会因帧号不等直接跳过，不会误判。
            const HashSlot& ls = hashLocal_[p.frame & HMASK];
            if (ls.frame == p.frame && ls.hash != 0 && ls.hash != rh)
                noteDesync(p.frame, ls.hash, rh);
            hashRemote_[p.frame & HMASK] = HashSlot{ p.frame, rh };
            continue;
        }

        const int maxCount = (n - int(sizeof(NetHeader))) / int(sizeof(NetEntry));
        const int cnt = std::min(int(h.count), maxCount);
        for (int k = 0; k < cnt; ++k) {
            NetEntry e;
            std::memcpy(&e, buf + sizeof(NetHeader) + size_t(k) * sizeof(NetEntry), sizeof(e));
            if (e.frame < remoteConsumed_) continue;
            Slot& s = remote_[e.frame & MASK];
            if (s.frame != e.frame) {
                s.frame = e.frame;
                s.input = e.input;
            }
        }

        lag_ = (h.slot >= slot_) ? int(h.slot - slot_) : -int(slot_ - h.slot);
    }
}

bool NetSession::waitForRemote(u32 frame, int timeoutMs) {
    const u32 deadline = nowMs() + u32(timeoutMs);
    u32 lastResend = 0;

    for (;;) {
        drainIncoming();
        if (disconnected_) return false;
        if (remote_[frame & MASK].frame == frame) return true;
        if (nowMs() >= deadline) return false;

        if (nowMs() - lastResend > 8) {     // 低频重发，抗丢包
            sendPacket(PT_INPUT);
            lastResend = nowMs();
        }
        sleepMs(1);
    }
}

u32 NetSession::beginFrame(u8 localInput) {
    if (role_ == Role::None) return NO_FRAME;

    const u32 s = slot_;
    local_[s & MASK].frame = s;
    local_[s & MASK].input = localInput;

    sendPacket(PT_INPUT);

    u32 result = NO_FRAME;
    if (s >= u32(delay_)) {
        const u32 target = s - u32(delay_);
        if (waitForRemote(target, 5000)) {
            result = target;
            remoteConsumed_ = target;
        } else {
            disconnected_ = true;
        }
    }
    slot_++;
    return result;
}

void NetSession::sendHash(u32 frame, u64 hash) {
    if (role_ == Role::None || !sock_.valid()) return;
    hashLocal_[frame & HMASK] = HashSlot{ frame, hash };

    NetHashPacket p{};
    std::memcpy(p.h.magic, "FCNP", 4);
    p.h.version = 1;
    p.h.type = PT_HASH;
    p.h.count = 1;
    p.h.slot = slot_;
    p.frame = frame;
    p.hashHi = u32(hash >> 32);
    p.hashLo = u32(hash & 0xFFFFFFFFull);
    sock_.sendTo(peerHost_, peerPort_, reinterpret_cast<const u8*>(&p), int(sizeof(p)));

    // 远端若已先到，这里补一次比对
    const HashSlot& rs = hashRemote_[frame & HMASK];
    if (rs.frame == frame && rs.hash != 0 && rs.hash != hash)
        noteDesync(frame, hash, rs.hash);
}

} // namespace fc
