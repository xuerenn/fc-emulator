// pausemenu.h — 游戏内暂停 / 设置覆盖层（F1 或 Esc 唤出）
//
// 设计成「非阻塞」而不是自带事件循环，是因为联机时必须继续推进帧：
// 锁步一旦停住，对端会一直等这一帧直到超时。所以菜单只是主循环里的一层绘制，
// 要不要暂停由调用方决定。
#pragma once

#include <SDL.h>

#include <string>

#include "ui.h"

namespace fc {

// 菜单里可能触发的操作
enum class PauseAction {
    None,
    Resume,
    SaveState,
    LoadState,
    KeyConfig,
    ToggleFullscreen,
    CycleScale,
    // 重启卡带：等价于按下主机的 Reset。多合一卡带（68 in 1 之类）开机先进自己的
    // 菜单，Reset 就又回到那个菜单 —— 想玩同一张卡带里的另一个游戏走这条，
    // 不用回启动器、更不用换卡带。
    ResetCart,
    // 退回启动器的列表换一张卡带。真正的动作在主循环里：按 autoSave 决定要不要
    // 先自动存一次档，再收掉本回合的联机/音频/纹理，让外层循环重新走一遍
    // 「挑卡带 → 跑游戏」。
    ToLibrary,
    Quit,
};

struct PauseInfo {
    std::string romName;
    int   fps = 0;

    // 联机状态
    bool  netActive  = false;
    bool  netRelayed = false;
    bool  desynced   = false;
    std::string netTag;          // "主机(1P)" / "中继·房间 1234"
    int   lag   = 0;
    int   delay = 0;

    // 本地状态
    bool  hasState   = false;    // 磁盘上有存档
    bool  fullscreen = false;
    int   scale      = 0;        // 0 = 自动
    bool  autoSave   = true;     // 离开当前进度前（重启卡带 / 换卡带）要不要自动存档

    // 一次性提示（调用方注入，例如「联机中不能读档」）
    std::string toast;
    bool        toastWarn = false;
};

// 绘制覆盖层并返回本帧用户触发的操作（None 表示无）
PauseAction pauseMenuFrame(ui::Ui& ui, const PauseInfo& info,
                           const ui::Input& in, double dt);

// 缩放档位的可读名称（自动 / 1x / ...），菜单与设置页共用
std::string scaleLabel(int scale);
int         nextScale(int scale);

} // namespace fc
