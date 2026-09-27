// net.h — 跨平台 UDP 与帧同步联机
#pragma once

#include "core/types.h"

#include <functional>

namespace fc {

// ================================================================ UDP 套接字
class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool open(u16 bindPort);
    void close();
    bool valid() const;

    bool sendTo(const std::string& host, u16 port, const u8* data, int n);
    // 返回 >0 数据长度，0 表示暂无数据，<0 表示出错
    int  recvFrom(std::string& outHost, u16& outPort, u8* data, int maxN);

private:
    std::intptr_t sock_ = -1;   // SOCKET / int，-1 表示无效
};

// ================================================================ 同步策略
// 目前实现「输入延迟锁步」；rollback 作为预留位，所需的全部底层能力
// （CPU/PPU/APU/Mapper 的 save/load、Emulator::saveState/loadState/stateHash）
// 均已就绪，补齐 RollbackStrategy::step() 即可启用。
class SyncStrategy {
public:
    virtual ~SyncStrategy() = default;
    virtual const char* name() const = 0;
    virtual bool wantsRollback() const { return false; }
    virtual int  defaultDelay() const { return 3; }
};

class LockstepStrategy : public SyncStrategy {
public:
    const char* name() const override { return "Lockstep(输入延迟帧同步)"; }
};

class RollbackStrategy : public SyncStrategy {
public:
    const char* name() const override { return "Rollback(回滚重模拟)"; }
    bool wantsRollback() const override { return true; }
    int  defaultDelay() const override { return 1; }
    // TODO: 实现需要
    //   1) 每帧 saveState() 存入环形快照池
    //   2) 本地输入立即前推模拟，收到远端输入后 loadState() 回滚并重模拟
    //   3) 预测远端输入（通常沿用上一帧输入）并在失配时回滚
};

// ================================================================ 中继配置
// 双方都主动连到一台公网中继服务器，由它配对并互相转发。
// 好处：两端都不需要公网 IP、不需要端口映射，任何 NAT 后面都能联机。
struct RelayConfig {
    std::string host;        // 中继服务器地址（点分十进制 IPv4）
    u16         port = 7777; // 中继服务器 UDP 端口
    std::string room = "7777";  // 房间号，两端必须一致
};

// 中继多了一跳，默认给更大的输入延迟才稳
constexpr int kRelayDefaultDelay = 6;

// ================================================================ 联机会话
//
// 存档同步（联机中读档）为什么必须做成「传档」而不是各自读各自的：
// 锁步要求两端在同一帧上状态逐位一致。只要两端的档有一丁点不同，下一个
// 哈希比对点就会报警，而且不可能自行收敛。所以由一方把快照原样传过去。
//
// 时序（发起方 = 数据源，接收方 = 照单全收）：
//   发起方 ── META + CHUNK… ──▶ 接收方   分块传；接收方按位图 ACK，缺块自动重传
//   发起方 ◀──── DONE ──────── 接收方   收齐且 CRC32 校验通过
//   发起方 重置采样序号（epoch+1，旧包全作废）
//   发起方 ────── GO ────────▶ 接收方   接收方此刻也重置序号、装上这份档
//   发起方 ◀──── APPLIED ───── 接收方
//   发起方 装上这份档，两端继续跑
//
// 非重置采样序号不可：传输期间两端停在不同帧上，序号已经错开；不重置的话
// 恢复后会把「不同的输入」喂给同一帧。重置 = 两端都从 0 重新起算，正好对齐。
// epoch 则负责把还在路上、序号属于旧体系的输入包全部丢掉。
class NetSession {
public:
    enum class Role { None, Host, Client };
    static constexpr u32 NO_FRAME = 0xFFFFFFFFu;

    NetSession() = default;
    ~NetSession();

    // 等待期间反复调用的回调（约每 2ms 一次），供前端泵事件、重绘
    // 「等待连接」画面；返回 false 表示用户取消，握手中止。
    using WaitHook = std::function<bool()>;

    // 阻塞式握手；失败返回 false
    bool startHost(u16 port, int inputDelay, std::string* err = nullptr,
                   const WaitHook& onWait = {});
    bool startClient(const std::string& host, u16 port, int inputDelay, std::string* err = nullptr,
                     const WaitHook& onWait = {});
    // 中继模式：本地不开端口，两端都主动连到中继
    bool startHostViaRelay(const RelayConfig& relay, int inputDelay, std::string* err = nullptr,
                           const WaitHook& onWait = {});
    bool startClientViaRelay(const RelayConfig& relay, int inputDelay, std::string* err = nullptr,
                             const WaitHook& onWait = {});
    // 结束会话。announceLibrary = true 时给对端发的断开包带上「我去换卡带了」的原因，
    // 对端前端收到后会跟着回启动器，而不是切回单机干等。
    void stop(bool announceLibrary = false);

    bool active() const { return role_ != Role::None; }
    Role role() const { return role_; }
    bool disconnected() const { return disconnected_; }
    bool desynced() const { return desynced_; }
    int  inputDelay() const { return delay_; }
    int  lag() const { return lag_; }
    const std::string& peer() const { return peerHost_; }

    // 中继模式相关
    bool relayed() const { return relayed_; }
    const RelayConfig& relayConfig() const { return relayCfg_; }

    // 卡带指纹（FNV-1a 64，整个 ROM 文件）。握手完成后两端会互发一次做校验，
    // 不一致直接报「两端的卡带不是同一份」并拒绝建立会话 ——
    // 不然「各自选卡带后重连」选错了，只会看到一句莫名其妙的「检测到状态不同步」。
    void setLocalRomHash(u64 h) { localRomHash_ = h; }
    bool peerWentToLibrary() const { return peerToLibrary_; }

    // 每帧调用一次：
    //   1) 记录并广播本帧物理输入
    //   2) 等待 delay 帧之前那一帧的远端输入
    //   3) 返回「本轮应该模拟的帧号」，NO_FRAME 表示还在预热/等待
    u32 beginFrame(u8 localInput);

    u8  localInput(u32 frame) const  { return local_.at(frame & MASK).input; }
    u8  remoteInput(u32 frame) const { return remote_.at(frame & MASK).input; }

    // 状态校验（每 N 帧调用一次，用于检测不同步）
    void sendHash(u32 frame, u64 hash);

    // 不同步事件详情：定位到底是哪一帧、两端的哈希各是多少
    struct DesyncInfo {
        u32 frame  = 0;
        u64 local  = 0;   // 本机在该帧算出的哈希
        u64 remote = 0;   // 对端报来的哈希
    };

    // 取出「上次调用之后新产生」的不同步事件；没有新事件则返回 false。
    // 主循环每帧都会轮询，若不做成一次性消费，同一次不同步会被打印几十遍刷屏。
    bool takeDesyncEvent(DesyncInfo& out);
    u32  desyncCount() const { return desyncCount_; }

    const SyncStrategy& strategy() const { return *strategy_; }

    // ------------------------------------------------ 存档同步（联机中读档）
    // 发起：本机是数据源，把 snapshot 同步给对端，最后两端都跑这一份档。
    // 传完之前本机不能推进模拟（stateSyncing() 为真时主循环必须停帧），
    // 否则两边会各自跑飞。
    void beginStateSync(const std::vector<u8>& snapshot);
    bool stateSyncing() const { return xfer_ != Xfer::None; }
    bool stateSending() const { return xfer_ == Xfer::Sending; }
    int  statePercent() const;
    const std::string& stateError() const { return xferErr_; }

    // 非阻塞推进收发。返回 true = 本帧要把 out 灌进 Emulator（每次同步只会为真一次）。
    bool pumpStateSync(std::vector<u8>& out);
    // 收尾（成功或失败都走这里），退回 Idle
    void endStateSync();

private:
    struct Slot { u32 frame = 0xFFFFFFFFu; u8 input = 0; };

    // 哈希必须与输入分开存：输入槽按 512 帧循环复用，
    // 若共用同一个槽，槽被复用时会「更新帧号但遗留旧哈希」，
    // 比对时就变成拿新帧的哈希去对 512 帧前的旧哈希 —— 必然误报。
    // 独立哈希环里的条目自带帧号，被复用时旧条目因帧号对不上而自动失效。
    struct HashSlot { u32 frame = 0xFFFFFFFFu; u64 hash = 0; };

    static constexpr int  BUF  = 512;
    static constexpr u32  MASK = BUF - 1;
    static constexpr int  HBUF = 512;
    static constexpr u32  HMASK = HBUF - 1;
    static constexpr int  kRedundancy = 16;   // 每个包携带最近 N 帧输入

    void noteDesync(u32 frame, u64 mine, u64 theirs);

    bool handshakeHost(std::string* err, const WaitHook& onWait);
    bool handshakeClient(const std::string& host, u16 port, std::string* err,
                         const WaitHook& onWait);
    // 握手成功后互发一次卡带指纹；不一致或等不到都算建会话失败
    bool exchangeRomHash(std::string* err, const WaitHook& onWait);
    bool sendPacket(u8 type);
    void drainIncoming();
    bool waitForRemote(u32 frame, int timeoutMs);

    // 中继：注册房间并等待配对（可被 onWait 中断）
    bool registerWithRelay(std::string* err, const WaitHook& onWait);
    void sendRelayControl(u8 type);
    void resetStreamState();

    UdpSocket sock_;
    Role role_ = Role::None;
    std::string peerHost_;
    u16  peerPort_ = 0;
    int  delay_ = 3;
    u32  slot_ = 0;
    u32  remoteConsumed_ = 0;
    bool disconnected_ = false;
    bool desynced_ = false;
    int  lag_ = 0;
    u32  lastResendMs_ = 0;

    // 中继状态
    RelayConfig relayCfg_;
    bool relayed_ = false;
    u64  localRomHash_  = 0;      // 本机卡带指纹（前端在 start* 前设置；0 = 不校验）
    u64  peerRomHash_   = 0;
    bool peerToLibrary_ = false;  // 对端断开的原因是「去换卡带了」
    bool relayReachable_ = false;   // 中继至少回过一次包（用于区分「服务器不通」和「对端没来」）

    std::array<Slot, BUF> local_{};
    std::array<Slot, BUF> remote_{};
    std::array<HashSlot, HBUF> hashLocal_{};
    std::array<HashSlot, HBUF> hashRemote_{};

    DesyncInfo desyncInfo_{};
    bool desyncPending_ = false;   // 有未播报的不同步事件
    u32  desyncCount_ = 0;

    // ---- 存档同步的状态机
    enum class Xfer { None, Sending, Receiving };
    enum class XferStage {
        Idle,
        SendChunks,    // 发送方：推窗口、等 ACK
        WaitDone,      // 发送方：数据发完，等接收方报「收齐了」
        WaitApplied,   // 发送方：已发 GO，等接收方报「装上了」
        RecvChunks,    // 接收方：攒块
        WaitGo,        // 接收方：攒齐了，等 GO
    };

    void handleStatePacket(u8 type, const u8* buf, int n);
    void pumpStateSend();
    void sendStateMeta();            // 开场包：告诉对端「我要发档，多大、校验和多少」
    void sendStateAck();
    void sendStateCtl(u8 type);
    void failStateSync(const char* why);
    bool xferTimedOut(u32 now);

    Xfer      xfer_  = Xfer::None;
    XferStage stage_ = XferStage::Idle;
    std::vector<u8> xferOut_;      // 发送方：待发的快照
    std::vector<u8> xferIn_;       // 接收方：拼装缓冲
    std::vector<u8> xferFlag_;     // 接收方：逐块到位标记
    u32  xferTotal_   = 0;         // 快照总字节
    u32  xferChunks_  = 0;         // 总块数
    u32  xferCrc_     = 0;         // 快照 CRC32
    u32  xferBase_    = 0;         // 发送方：窗口起点（块号）
    u64  xferAckBits_ = 0;         // 发送方：窗口内已确认的块（相对 xferBase_）
    u32  xferGot_     = 0;         // 接收方：已到位的块数
    u32  xferRecvBase_ = 0;        // 接收方：最小的还没到位的块号（回执里的 base）
    u32  xferLastTx_  = 0;
    u32  xferLastCtl_ = 0;
    u32  xferStartMs_ = 0;
    u8   xferEpoch_   = 0;         // 同步完成后启用的新代次
    bool xferApply_   = false;     // 本帧该装档（pumpStateSync 取走）
    bool xferStarted_ = false;     // 对端已回应过（说明它收到了开场包，进入了接收态）
    std::string xferErr_;

    // 输入包代次。重置采样序号时必须 +1：传输期间两端停在不同帧上，旧包还在路上，
    // 它们带的帧号在新体系里会正好落进环形缓冲的空位，被当成有效输入。
    u8   epoch_ = 0;

    std::unique_ptr<SyncStrategy> strategy_;
};

// 供 UI 显示
const char* netRoleName(NetSession::Role r);

} // namespace fc
