// relay_proto.h — 模拟器 ⇄ 中继服务器 之间的控制协议
//
// 这是本工程唯一的「线路格式」定义处。net.cpp（客户端侧）与 relay.cpp（服务器侧）
// 都包含它，所以两边永远不会因为手抄结构体而写歪。
//
// 设计要点：
//   * 中继是**透明**的：配对完成后它只是把包原样转发给另一端，
//     模拟器自身的 "FCNP" 报文完全不需要中继理解。
//   * 中继只处理魔数为 "FCRL" 的控制包，其余包一律按业务流量转发。
//   * 房间号是两端唯一的共同秘密；同一个房间号只能容纳两个端点。
#pragma once

#include <cstdint>

namespace fc {
namespace relay {

// 控制报文魔数。模拟器的联机包用 "FCNP"，两者互不干扰。
constexpr char kMagic[4] = { 'F', 'C', 'R', 'L' };
constexpr std::uint8_t kVersion = 1;

// 房间号最大长度（含结尾 '\0'）
constexpr int kRoomLen = 32;

enum Type : std::uint8_t {
    RL_REGISTER   = 1,   // 端 -> 中继：注册房间（未配对时每 200ms 重发）
    RL_WAIT       = 2,   // 中继 -> 端：已登记，等对端。仅用于确认「中继可达」
    RL_PAIR       = 3,   // 中继 -> 端：两端就位，可以开始握手
    RL_BUSY       = 4,   // 中继 -> 端：房间已被别的玩家占用
    RL_UNREGISTER = 5,   // 端 -> 中继：主动离开，让服务器立刻回收槽位
};

// 角色提示。中继只用它把日志标成「主机/客机」，不影响转发行为。
enum Role : std::uint8_t {
    RL_ROLE_NONE   = 0,
    RL_ROLE_HOST   = 1,
    RL_ROLE_CLIENT = 2,
};

#pragma pack(push, 1)
struct Header {
    char          magic[4];
    std::uint8_t  version;
    std::uint8_t  type;
    std::uint8_t  role;
    std::uint8_t  reserved;
    char          room[kRoomLen];   // 以 '\0' 结尾的房间号
};
#pragma pack(pop)

static_assert(sizeof(Header) == 40, "relay::Header 布局必须稳定为 40 字节");

} // namespace relay
} // namespace fc
