// keycfg.h — 键位映射（默认 WASD + JK），可改、可存盘
//
// 设计要点：
//   1) 存的是 SDL 扫描码（scancode）而不是键码：扫描码对应物理按键位置，
//      不受键盘布局/输入法影响，存盘后换机器也能稳定还原。
//   2) 每个动作只绑定一个键，界面和配置文件都简单直白。
//   3) 配置文件写成「动作=键名」的纯文本，用 SDL 自己的键名，
//      所以既能被程序写回、也能被用户手改。
#pragma once

#include "core/types.h"
#include "core/controller.h"   // BTN_* 位定义（与手柄寄存器共用同一套位序）

#include <SDL.h>

#include <array>
#include <string>

namespace fc {

// 一个手柄的 8 个动作。顺序就是界面里从上到下的顺序。
enum PadAction {
    ACT_UP = 0,
    ACT_DOWN,
    ACT_LEFT,
    ACT_RIGHT,
    ACT_A,
    ACT_B,
    ACT_SELECT,
    ACT_START,
    ACT_COUNT
};

constexpr int kPlayerCount = 2;

// 动作 → 手柄位掩码（与 core/controller.h 的 Button 一致）
u8 actionButton(int act);

// 界面/日志用的短名
const char* actionLabel(int act);      // "UP" "DOWN" ... "START"
const char* actionLabelCN(int act);    // "上" "下" ...

struct KeyMap {
    std::array<std::array<SDL_Scancode, ACT_COUNT>, kPlayerCount> keys{};

    void setDefaults();

    SDL_Scancode get(int player, int act) const;
    void set(int player, int act, SDL_Scancode sc);

    // 该键是否已被同一玩家的别的动作占用；返回动作号，没有返回 -1
    int conflictIn(int player, SDL_Scancode sc, int exceptAct = -1) const;
    // 是否与另一个玩家重复（本地双人时按同一个键会同时触发两人，仅提示不拦截）
    bool overlapsOtherPlayer(int player, SDL_Scancode sc) const;

    // 读键盘当前状态 → 该玩家的手柄位掩码
    u8 sample(int player) const;

    // 配置文件读写；读失败保持默认值（首次运行就是这种情况，属于正常）
    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

// 键名的可读文本；SDL 无对应名字时回退为 "(NONE)"
std::string keyName(SDL_Scancode sc);

// 默认配置文件路径：<exe 所在目录>/fc-keys.cfg（取不到就退回当前目录）
std::string defaultKeyConfigPath();

} // namespace fc
