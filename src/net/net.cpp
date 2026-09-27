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
    // 输入包代次。平时恒为 0；每次「重置采样序号」（存档同步结束时）两端一起 +1，
    // 好把还在路上的旧序号输入包全部作废。只对 PT_INPUT / PT_HASH 生效 ——
    // 同步握手那几个包必须无视代次，否则两端会互相把对方的握手包丢掉。
    u8  epoch;
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

// ---- 存档同步
constexpr int kStateChunk  = 1024;   // 单块净荷（加上头也只有 1KB 出头，不会被 IP 分片）
constexpr int kStateWindow = 64;     // 滑动窗口（块数）：64KB 在途

struct NetStateMetaPacket {
    NetHeader h;
    u32 totalSize;
    u32 crc32;
    u8  newEpoch;        // 同步完成后两端要启用的代次
    u8  pad[3];
};
struct NetStateChunkPacket {
    NetHeader h;
    u16 index;
    u16 len;
    u8  payload[kStateChunk];
};
// 累计确认 + 窗口内位图（选择性重传）。base 之前的块都收齐了。
struct NetStateAckPacket {
    NetHeader h;
    u32 base;
    u64 bitmap;
};
struct NetStateCtlPacket {
    NetHeader h;
    u8  newEpoch;
    u8  pad[3];
    u32 crc32;           // DONE 时带快照校验和，APPLIED / ABORT 时为 0
};
// 握手后的卡带指纹交换。指纹对不上就当场拒绝建会话，
// 不让「两端选了不同卡带」拖到第一帧哈希比对才爆出来。
struct NetRomIdPacket {
    NetHeader h;
    u64 hash;
};
#pragma pack(pop)

enum PacketType : u8 {
    PT_HELLO     = 1,
    PT_HELLO_ACK = 2,
    PT_CONFIRM   = 3,
    PT_INPUT     = 4,
    PT_HASH      = 5,
    PT_BYE       = 6,
    PT_STATE_META    = 7,
    PT_STATE_CHUNK   = 8,
    PT_STATE_ACK     = 9,
    PT_STATE_DONE    = 10,   // 接收方：数据收齐且校验通过
    PT_STATE_GO      = 11,   // 发送方：重置完序号了，你那边也重置并装档
    PT_STATE_APPLIED = 12,   // 接收方：装好了
    PT_STATE_ABORT   = 13,   // 任一方：这次同步作废
    PT_ROMID         = 14,   // 握手后互发一次：本机卡带指纹（u64）
    PT_BYE_CART      = 15,   // 断开原因 = 「我去换卡带了」，对端应跟着回启动器
};

// 快照校验用（多项式与 zlib 相同，但这里不引依赖，免得为 12 行代码去链一个库）
u32 crc32Of(const u8* p, size_t n) {
    static u32 table[256];
    static bool init = false;
    if (!init) {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

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
    // 代次也一起清掉：新会话从 0 开始，旧会话残留的包不该被认。
    epoch_ = 0;
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
    if (!exchangeRomHash(err, onWait)) { stop(); return false; }
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
    if (!exchangeRomHash(err, onWait)) { stop(); return false; }
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
    if (!exchangeRomHash(err, onWait)) { stop(); return false; }
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
    if (!exchangeRomHash(err, onWait)) { stop(); return false; }
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

void NetSession::stop(bool announceLibrary) {
    if (role_ != Role::None && !peerHost_.empty()) {
        // 正传着档就断了：告诉对端别等了，然后把状态清干净。
        if (xfer_ != Xfer::None) sendStateCtl(PT_STATE_ABORT);
        sendPacket(announceLibrary ? PT_BYE_CART : PT_BYE);
    }
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
    xfer_  = Xfer::None;
    stage_ = XferStage::Idle;
    xferOut_.clear();
    xferIn_.clear();
    xferFlag_.clear();
    xferErr_.clear();
    xferApply_ = false;
    peerRomHash_   = 0;
    peerToLibrary_ = false;
    epoch_ = 0;
}

bool NetSession::exchangeRomHash(std::string* err, const WaitHook& onWait) {
    // 前端没给指纹（例如某些内嵌用法）就跳过校验，保持旧行为
    if (localRomHash_ == 0 || !sock_.valid() || peerHost_.empty()) return true;

    const u32 deadline = nowMs() + 5000;    // UDP 会丢，重发 + 等待共给 5 秒
    u32 lastTx = 0;
    u8 buf[512];
    std::string rh;
    u16 rp = 0;

    while (nowMs() < deadline) {
        if (onWait && !onWait()) {
            if (err) *err = "已取消";
            return false;
        }
        if (nowMs() - lastTx > 250) {       // 低频重发，两边都在发，丢几包也收得到
            NetRomIdPacket p{};
            std::memcpy(p.h.magic, "FCNP", 4);
            p.h.version = 1;
            p.h.type    = PT_ROMID;
            p.h.epoch   = epoch_;
            p.h.slot    = slot_;
            p.hash      = localRomHash_;
            sock_.sendTo(peerHost_, peerPort_,
                         reinterpret_cast<const u8*>(&p), int(sizeof(p)));
            lastTx = nowMs();
        }
        const int n = sock_.recvFrom(rh, rp, buf, sizeof(buf));
        if (n >= int(sizeof(NetRomIdPacket))) {
            NetRomIdPacket p;
            std::memcpy(&p, buf, sizeof(p));
            if (std::memcmp(p.h.magic, "FCNP", 4) == 0 && p.h.version == 1 &&
                p.h.type == PT_ROMID && rh == peerHost_ && rp == peerPort_) {
                peerRomHash_ = p.hash;
                if (p.hash != localRomHash_) {
                    if (err) *err = "两端的卡带不是同一份 ROM（文件指纹不符）——"
                                    "联机要求双方运行字节完全相同的卡带，请确认选的是同一个游戏文件";
                    return false;
                }
                return true;
            }
        }
        sleepMs(2);
    }
    if (err) *err = "交换卡带指纹超时（网络丢包严重？）";
    return false;
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
    h.epoch = epoch_;
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
    // 得放得下最大的包：存档同步的分块包是 1KB 净荷，512 字节会把它们截断。
    u8 buf[2048];
    std::string host;
    u16 port = 0;

    for (int guard = 0; guard < 256; ++guard) {
        const int n = sock_.recvFrom(host, port, buf, sizeof(buf));
        if (n <= 0) break;
        if (n < int(sizeof(NetHeader))) continue;

        NetHeader h;
        std::memcpy(&h, buf, sizeof(h));
        if (std::memcmp(h.magic, "FCNP", 4) != 0 || h.version != 1) continue;
        if (!peerHost_.empty() && (host != peerHost_ || port != peerPort_)) continue;

        if (h.type == PT_BYE || h.type == PT_BYE_CART) {
            // 断开也分两种：普通退出（对端接着单机玩），和「我去换卡带了」
            // （对端会跟着回启动器，各自选好再重新连）。原因记下来给前端看。
            peerToLibrary_ = (h.type == PT_BYE_CART);
            disconnected_  = true;
            return;
        }

        // 存档同步的握手包不查代次 —— 两端正是在同步过程中换代的，
        // 查了就会把对方的关键包丢掉，然后一起卡死。
        if (h.type >= PT_STATE_META && h.type <= PT_STATE_ABORT) {
            handleStatePacket(h.type, buf, n);
            continue;
        }

        // 输入与哈希必须同代：同步结束后还在路上的旧包，帧号在新体系里会正好
        // 落进环形缓冲的空位，被当成有效输入用掉。
        if (h.epoch != epoch_) continue;

        if (h.type == PT_HASH && n >= int(sizeof(NetHashPacket))) {
            // 同步过程中两端的模拟本来就会先跑偏几帧（谁先停下来的时间不一样），
            // 这时候的哈希比对没有意义，报了也只是噪声。
            if (stateSyncing()) continue;
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
        // 对方发起了存档同步（可能是「他按了读档」），得立刻从等帧里退出来
        // 去处理传输。这里绝不能当作「超时断线」—— 那会把一次正常的同步
        // 变成一次误判掉线。
        if (stateSyncing()) return false;
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
    // 正在收发存档时一律不推进：套用旧序号去模拟只会让两端跑飞。
    if (stateSyncing()) return NO_FRAME;

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
        } else if (stateSyncing()) {
            // 等待期间对端发起了同步：本帧作废，但绝不能算断线。
            // 序号也别推进 —— 同步结束时两端会一起把序号归零。
            return NO_FRAME;
        } else {
            disconnected_ = true;
        }
    }
    slot_++;
    return result;
}

void NetSession::sendHash(u32 frame, u64 hash) {
    if (role_ == Role::None || !sock_.valid()) return;
    if (stateSyncing()) return;      // 同步期间的哈希没有可比性，别污染比对环
    hashLocal_[frame & HMASK] = HashSlot{ frame, hash };

    NetHashPacket p{};
    std::memcpy(p.h.magic, "FCNP", 4);
    p.h.version = 1;
    p.h.type = PT_HASH;
    p.h.count = 1;
    p.h.epoch = epoch_;      // 同步过之后代次不再是 0，漏了这行哈希包会被对端整批丢掉
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

// ================================================================ 存档同步
//
// 先看头文件里那张时序图。这里只强调一件事：**只有当两端都停在原地时**
// 这份档才是可用的，所以发起方一按就读档、然后停帧，接收方一收到 META 也停帧。
// 谁也不能一边收一边跑 —— 跑出来的状态立刻就和对方不一样了。
namespace {
constexpr u32 kXferTimeoutMs  = 20000;   // 应用握手之前的阶段
constexpr u32 kXferTailMs     = 60000;   // 已经发过 GO / 收齐数据之后（此时已经没法回退了）
constexpr u32 kXferRetxMs     = 60;      // 窗口重推间隔
constexpr u32 kXferAckMs      = 6;       // 接收方最多这么回一次 ACK
constexpr u32 kMaxStateBytes  = 16u * 1024 * 1024;
constexpr u32 kMinCtlMs       = 150;     // GO / DONE 的重发间隔
} // namespace

void NetSession::beginStateSync(const std::vector<u8>& snapshot) {
    if (role_ == Role::None || !sock_.valid() || peerHost_.empty()) return;
    if (xfer_ != Xfer::None || snapshot.empty()) return;
    if (snapshot.size() > kMaxStateBytes) return;

    xferOut_     = snapshot;
    xferTotal_   = u32(snapshot.size());
    xferChunks_  = (xferTotal_ + u32(kStateChunk) - 1) / u32(kStateChunk);
    xferCrc_     = crc32Of(snapshot.data(), snapshot.size());
    xferBase_    = 0;
    xferAckBits_ = 0;
    xferEpoch_   = u8(epoch_ + 1);      // 同步完成后两端一起换到这个代次
    xferErr_.clear();
    xferApply_   = false;
    xferStarted_ = false;
    xfer_        = Xfer::Sending;
    stage_       = XferStage::SendChunks;
    xferStartMs_ = nowMs();
    xferLastTx_  = 0;
    xferLastCtl_ = nowMs();

    std::printf("[同步] 把本机存档发给对方：%u 字节 / %u 块\n", xferTotal_, xferChunks_);
    std::fflush(stdout);
    // 顺序不能反：必须先让对端知道「要收档、多大、校验和多少」，
    // 否则它还在 None 状态，后面所有数据块都会被当成无关包丢掉。
    sendStateMeta();
    pumpStateSend();                    // 立刻推第一窗口，别干等一帧
}

int NetSession::statePercent() const {
    if (xfer_ == Xfer::None || xferChunks_ == 0) return 0;
    const u32 done = (xfer_ == Xfer::Sending) ? xferBase_ : xferGot_;
    return int(std::min<u32>(100u, (done * 100u) / xferChunks_));
}

void NetSession::endStateSync() {
    xfer_       = Xfer::None;
    stage_      = XferStage::Idle;
    xferApply_  = false;
    xferOut_.clear();
    xferIn_.clear();
    xferFlag_.clear();
}

void NetSession::failStateSync(const char* why) {
    if (xfer_ == Xfer::None) return;
    xferErr_ = why;
    std::fprintf(stderr, "[同步] 中止：%s\n", why);
    std::fflush(stderr);
    sendStateCtl(PT_STATE_ABORT);
    endStateSync();
}

bool NetSession::xferTimedOut(u32 now) {
    if (xfer_ == Xfer::None) return false;
    // 进了 GO / DONE 之后两端已经被动过采样序号，退不回去了，只能多给点时间。
    const bool tail = (stage_ == XferStage::WaitApplied || stage_ == XferStage::WaitGo);
    return now - xferStartMs_ > (tail ? kXferTailMs : kXferTimeoutMs);
}

void NetSession::sendStateCtl(u8 type) {
    if (!sock_.valid() || peerHost_.empty()) return;
    NetStateCtlPacket p{};
    std::memcpy(p.h.magic, "FCNP", 4);
    p.h.version = 1;
    p.h.type    = type;
    p.h.epoch   = epoch_;
    p.h.slot    = slot_;
    p.newEpoch  = xferEpoch_;
    p.crc32     = (type == PT_STATE_DONE) ? xferCrc_ : 0;
    sock_.sendTo(peerHost_, peerPort_, reinterpret_cast<const u8*>(&p), int(sizeof(p)));
}

void NetSession::sendStateMeta() {
    if (!sock_.valid() || peerHost_.empty()) return;
    NetStateMetaPacket p{};
    std::memcpy(p.h.magic, "FCNP", 4);
    p.h.version = 1;
    p.h.type    = PT_STATE_META;
    p.h.epoch   = epoch_;
    p.h.slot    = slot_;
    p.totalSize = xferTotal_;
    p.crc32     = xferCrc_;
    p.newEpoch  = xferEpoch_;
    sock_.sendTo(peerHost_, peerPort_, reinterpret_cast<const u8*>(&p), int(sizeof(p)));
}

// 接收方回执：base = 最小的还没收到的块号；bitmap 的第 i 位 = 块 base+i 已到位。
void NetSession::sendStateAck() {
    if (xfer_ != Xfer::Receiving) return;
    while (xferRecvBase_ < xferChunks_ && xferFlag_[xferRecvBase_]) ++xferRecvBase_;

    NetStateAckPacket p{};
    std::memcpy(p.h.magic, "FCNP", 4);
    p.h.version = 1;
    p.h.type    = PT_STATE_ACK;
    p.h.epoch   = epoch_;
    p.h.slot    = slot_;
    p.base      = xferRecvBase_;
    for (u32 i = 0; i < 64; ++i) {
        const u32 idx = xferRecvBase_ + i;
        if (idx < xferChunks_ && xferFlag_[idx]) p.bitmap |= (1ull << i);
    }
    sock_.sendTo(peerHost_, peerPort_, reinterpret_cast<const u8*>(&p), int(sizeof(p)));
}

void NetSession::pumpStateSend() {
    if (xfer_ != Xfer::Sending || xferOut_.empty()) return;
    if (xferBase_ >= xferChunks_) {
        // 全确认了，转入等对端 DONE 的阶段。
        // 这一步不能省：DONE 只在 WaitDone 阶段受理，停在 SendChunks 的话
        // 对端报来的 DONE 会被直接丢掉，两端就只能干等到超时。
        if (stage_ == XferStage::SendChunks) {
            stage_       = XferStage::WaitDone;
            xferLastCtl_ = nowMs();
        }
        return;
    }

    const u32 now = nowMs();
    if (xferLastTx_ != 0 && now - xferLastTx_ < kXferRetxMs) return;
    xferLastTx_ = now;

    NetStateChunkPacket pkt{};
    std::memcpy(pkt.h.magic, "FCNP", 4);
    pkt.h.version = 1;
    pkt.h.type    = PT_STATE_CHUNK;
    pkt.h.epoch   = epoch_;
    pkt.h.slot    = slot_;

    const u32 end = std::min(xferChunks_, xferBase_ + u32(kStateWindow));
    for (u32 i = xferBase_; i < end; ++i) {
        const u32 bit = i - xferBase_;
        if (bit < 64 && ((xferAckBits_ >> bit) & 1ull)) continue;   // 已确认，不必重发
        const u32 off = i * u32(kStateChunk);
        const u32 len = std::min(u32(kStateChunk), xferTotal_ - off);
        pkt.index = u16(i);
        pkt.len   = u16(len);
        std::memcpy(pkt.payload, xferOut_.data() + off, len);
        sock_.sendTo(peerHost_, peerPort_, reinterpret_cast<const u8*>(&pkt),
                     int(sizeof(NetHeader) + 4 + len));
    }
}

void NetSession::handleStatePacket(u8 type, const u8* buf, int n) {
    const u32 now = nowMs();

    switch (type) {
    case PT_STATE_META: {
        if (n < int(sizeof(NetStateMetaPacket))) return;
        NetStateMetaPacket p;
        std::memcpy(&p, buf, sizeof(p));
        if (xfer_ != Xfer::None) {
            // 两边同时按了读档。谁赢都行，但不能两边同时当发送方 —— 直接拒掉，
            // 让用户在数秒后重试一次。
            sendStateCtl(PT_STATE_ABORT);
            return;
        }
        if (p.totalSize == 0 || p.totalSize > kMaxStateBytes) { sendStateCtl(PT_STATE_ABORT); return; }

        xferChunks_ = (p.totalSize + u32(kStateChunk) - 1) / u32(kStateChunk);
        xferTotal_  = p.totalSize;
        xferCrc_    = p.crc32;
        xferEpoch_  = p.newEpoch;
        xferIn_.assign(size_t(p.totalSize), 0);
        xferFlag_.assign(xferChunks_, 0);
        xferGot_      = 0;
        xferRecvBase_ = 0;
        xferLastCtl_  = 0;
        xferErr_.clear();
        xferApply_  = false;
        xfer_       = Xfer::Receiving;
        stage_      = XferStage::RecvChunks;
        xferStartMs_ = now;
        std::printf("[同步] 对方正在把存档发过来：%u 字节 / %u 块\n", xferTotal_, xferChunks_);
        std::fflush(stdout);
        sendStateAck();                 // 先回一个，让发起方立刻敢推第一窗口
        return;
    }

    case PT_STATE_CHUNK: {
        if (xfer_ != Xfer::Receiving || stage_ != XferStage::RecvChunks) return;
        if (n < int(sizeof(NetHeader)) + 4) return;

        u16 idx = 0, len = 0;
        std::memcpy(&idx, buf + sizeof(NetHeader), 2);
        std::memcpy(&len, buf + sizeof(NetHeader) + 2, 2);
        if (idx >= xferChunks_ || len > kStateChunk) return;
        if (n < int(sizeof(NetHeader)) + 4 + int(len)) return;

        if (!xferFlag_[idx]) {
            const u32 off     = u32(idx) * u32(kStateChunk);
            const u32 copyLen = std::min(u32(len), xferTotal_ - off);
            std::memcpy(xferIn_.data() + off, buf + sizeof(NetHeader) + 4, copyLen);
            xferFlag_[idx] = 1;
            ++xferGot_;
        }

        if (xferGot_ == xferChunks_) {
            // 先校验再报喜：UDP 不保证内容，传错一份档比传不动更糟。
            if (crc32Of(xferIn_.data(), xferIn_.size()) != xferCrc_) {
                failStateSync("存档在传输中损坏（校验和不符）");
                return;
            }
            stage_ = XferStage::WaitGo;
            xferLastCtl_ = now;
            sendStateCtl(PT_STATE_DONE);
            std::printf("[同步] 存档收齐且校验通过，等对方放行\n");
            std::fflush(stdout);
            return;
        }
        if (now - xferLastCtl_ >= kXferAckMs) { xferLastCtl_ = now; sendStateAck(); }
        return;
    }

    case PT_STATE_ACK: {
        if (xfer_ != Xfer::Sending) return;
        if (n < int(sizeof(NetStateAckPacket))) return;
        NetStateAckPacket p;
        std::memcpy(&p, buf, sizeof(p));
        xferStarted_ = true;                        // 对端进入接收态了，不用再重发开场包
        if (p.base < xferBase_) return;             // 迟到的旧回执
        if (p.base > xferBase_) {
            const u32 shift = p.base - xferBase_;
            xferAckBits_ = (shift >= 64) ? 0ull : (xferAckBits_ >> shift);
            xferBase_    = p.base;
        }
        xferAckBits_ |= p.bitmap;
        while ((xferAckBits_ & 1ull) && xferBase_ < xferChunks_) {
            xferAckBits_ >>= 1;
            ++xferBase_;
        }
        return;
    }

    case PT_STATE_DONE: {
        if (xfer_ != Xfer::Sending) return;
        if (n >= int(sizeof(NetStateCtlPacket))) {
            NetStateCtlPacket p;
            std::memcpy(&p, buf, sizeof(p));
            if (p.crc32 != xferCrc_) {
                // 对端拼出来的和我们发的不是一份（多半是上一轮迟到的旧 DONE）。
                // 再推一遍窗口，等它把真正的那次收齐报上来。
                if (xferBase_ < xferChunks_) { xferLastTx_ = 0; pumpStateSend(); }
                return;
            }
        }
        // 对端已经收齐并且校验通过，那它手上的就是这份档。
        // 本机这边最后一次回执丢了也不影响 —— 强行把记账推到终点再放行，
        // 否则会出现「我以为还有几块没确认」而一直重推、对端又早已停止收块的对峙。
        xferBase_    = xferChunks_;
        xferAckBits_ = 0;
        xferStarted_ = true;
        stage_       = XferStage::WaitDone;
        // 顺序很要紧：先归零采样序号并换代，再让对方 GO。
        // 反过来的话，对方会抢在我们前面用新序号发包，而我们还在旧体系里，全被丢掉。
        resetStreamState();
        epoch_       = xferEpoch_;
        stage_       = XferStage::WaitApplied;
        xferLastCtl_ = now;
        sendStateCtl(PT_STATE_GO);
        std::printf("[同步] 对方已收齐；本机采样序号已归零，等待握手完成\n");
        std::fflush(stdout);
        return;
    }

    case PT_STATE_GO: {
        if (xfer_ != Xfer::Receiving || stage_ != XferStage::WaitGo) return;
        resetStreamState();
        epoch_ = xferEpoch_;
        sendStateCtl(PT_STATE_APPLIED);
        xferApply_ = true;                          // 交给主循环装档
        stage_     = XferStage::Idle;
        return;
    }

    case PT_STATE_APPLIED: {
        if (xfer_ != Xfer::Sending || stage_ != XferStage::WaitApplied) return;
        xferApply_ = true;      // 发起方也要装：不然对方跑新档、本机跑旧档，立刻不同步
        stage_     = XferStage::Idle;
        return;
    }

    case PT_STATE_ABORT: {
        if (xfer_ == Xfer::None) return;
        // 别再回一个 ABORT 回去，否则两端会互相中止个没完。
        xferErr_ = "对方中止了本次同步";
        std::fprintf(stderr, "[同步] 中止：%s\n", xferErr_.c_str());
        std::fflush(stderr);
        xfer_      = Xfer::None;
        stage_     = XferStage::Idle;
        xferApply_ = false;
        xferOut_.clear();
        xferIn_.clear();
        xferFlag_.clear();
        return;
    }

    default:
        return;
    }
}

bool NetSession::pumpStateSync(std::vector<u8>& out) {
    if (xfer_ == Xfer::None) return false;
    const u32 now = nowMs();

    // 自己也收一遍：调用方这一刻可能没在等帧，包不能积在系统缓冲里。
    drainIncoming();
    if (xfer_ == Xfer::None) return false;          // drain 里可能已经判失败了

    if (xferTimedOut(now)) {
        if (stage_ == XferStage::WaitApplied) {
            // 已经发过 GO、序号也归零了，退不回去。对方多半是收到了 GO 但回执全丢，
            // 那它手上就是这份档 —— 本机跟着装上，至少两端跑的还是同一份。
            std::fprintf(stderr, "[同步] 等不到对方回执，但序号已重置，本机照样装上这份档；\n"
                                 "       若两端表现不一致，请重新联机一次。\n");
            xferApply_   = true;
            xferLastCtl_ = now;
        } else {
            failStateSync("同步超时（网络不通或丢包太严重）");
            return false;
        }
    }

    if (xfer_ == Xfer::Sending) {
        // 开场包走 UDP，丢了就永远等不到对端的回执。没收到回应前低频重发。
        if (!xferStarted_ && now - xferLastCtl_ > kMinCtlMs) {
            xferLastCtl_ = now;
            sendStateMeta();
        }
        if (stage_ == XferStage::SendChunks) {
            pumpStateSend();
        } else if (stage_ == XferStage::WaitDone) {
            // 尾部那几块可能还没到，按低频再推一遍
            if (now - xferLastCtl_ > 300) {
                xferLastCtl_ = now;
                xferLastTx_  = 0;
                pumpStateSend();
            }
        } else if (stage_ == XferStage::WaitApplied) {
            if (now - xferLastCtl_ > kMinCtlMs) { xferLastCtl_ = now; sendStateCtl(PT_STATE_GO); }
        }
    } else if (xfer_ == Xfer::Receiving) {
        if (stage_ == XferStage::WaitGo && now - xferLastCtl_ > kMinCtlMs) {
            xferLastCtl_ = now;
            sendStateCtl(PT_STATE_DONE);            // DONE 万一丢了就再报一次
        }
    }

    if (xferApply_) {
        xferApply_ = false;
        out = (xfer_ == Xfer::Sending) ? xferOut_ : xferIn_;
        stage_ = XferStage::Idle;
        return true;
    }
    return false;
}

} // namespace fc
